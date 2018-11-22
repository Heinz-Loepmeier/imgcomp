//-----------------------------------------------------------------------------------
// Simple tool for monitor continuously captured images for changes
// and saving changed images, as well as an image every N seconds for timelapses.
// Matthias Wandel 2015
//
// Imgcomp is licensed under GPL v2 (see README.txt)
//-----------------------------------------------------------------------------------
#include <stdio.h>
#include <ctype.h>		// to declare isupper(), tolower() 
#include <stdlib.h>
//#include <sys/types.h>
//#include <sys/stat.h>
#include <string.h>
//#include <time.h>
#ifdef _WIN32
//    #include "readdir.h"
//    #define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
//    #define strdup(a) _strdup(a) 
//    extern void sleep(int);
//    #define unlink(n) _unlink(n)
#else
    #include <dirent.h>
    #include <unistd.h>
#endif

#include "imgcomp.h"
#include "config.h"

//-----------------------------------------------------------------------------------
// Indicate command line usage.
//-----------------------------------------------------------------------------------
void usage (void)// complain about bad command line 
{
    fprintf(stderr, "usage: %s [switches] ", progname);
    fprintf(stderr, "inputfile outputfile\n");

    fprintf(stderr, 
     "Switches (names may be abbreviated):\n"
     " -scale   N           Scale before detection by 1/N.  Default 1/4\n"
     " -region  x1-x2,y1-y2 Specify region of interest\n"
     " -exclude x1-x2,y1-y2 Exclude from area of interest\n"
     " -diffmap <filename>  A file to use as diff map\n"
     " -dodir   <srcdir>    Compare images in dir, in order\n"
     " -followdir <srcdir>  Do dir and monitor for new images\n"
     " -savedir <saveto>    Where to save images with changes\n"
     " -savenames <scheme>  Output naming scheme.  Uses strftime\n"
     "                      to format the output name.  May include\n"
     "                      '/' characters for directories.\n"
     " -sensitivity N       Set sensitivity.  Lower=more sensitive\n"
     " -blink_cmd <command> Run this command when motion detected\n"
     "                      (used to blink the camera LED)\n"
     " -tl N                Save image every N seconds regardless\n"
     " -spurious            Ignore any change that returns to\n"
     "                      previous image in the next frame\n"
     " -sendsigs            Send signal to raspistil every second\n"
     "                      For continous AE rapsistill, default off\n"
     " -brmonitor           Restart raspistill on brightness\n"
     "                      changes (default on)\n"
     " -verbose or -debug   Emit more verbose output\n"
     " -logtofile           Log to file instead of stdout\n"
     " -movelognames <schme> Rotate log files, scheme works just like\n"
     "                      it does for savenames\n");
   
    exit(-1);
}

//-----------------------------------------------------------------------------------
// Case-insensitive matching of possibly-abbreviated keyword switches.
// keyword is the constant keyword (must be lower case already),
// minchars is length of minimum legal abbreviation.
//-----------------------------------------------------------------------------------
static int keymatch (const char * arg, const char * keyword, int minchars)
{
    int ca, ck, nmatched = 0;

    while ((ca = *arg++) != '\0') {
        if ((ck = *keyword++) == '\0') return 0;  // arg longer than keyword, no good */
        if (isupper(ca))  ca = tolower(ca);	 // force arg to lcase (assume ck is already)
        if (ca != ck)  return 0;             // no good 
        nmatched++;			                 // count matched characters 
    }
    // reached end of argument; fail if it's too short for unique abbrev 
    if (nmatched < minchars) return 0;
    return 1;
}

//-----------------------------------------------------------------------------------
// Parse a region parameter.
//-----------------------------------------------------------------------------------
static int ParseRegion(Region_t * reg, const char * value)
{
    char * t;
    t = strstr(value, ",");
    if (t == NULL || t != value){
        // No comma, or comma not in first position.  Parse X parameters.
        if (sscanf(value, "%d-%d", &reg->x1, &reg->x2) != 2){
            if (sscanf(value, "%d+%d", &reg->x1, &reg->x2) != 2) return 0;
            reg->x2 += reg->x1;
        }
    }
    if (t != NULL){
        // Y parameters come after the comma.
        if (sscanf(t+1, "%d-%d", &reg->y1, &reg->y2) != 2){
            if (sscanf(t+1, "%d+%d", &reg->y1, &reg->y2) != 2) return 0;
            reg->y2 += reg->y1;
        }
    }
    if (reg->y2-reg->y1 < 16 || reg->x2-reg->x1 < 16){
        fprintf(stderr,"Detect region is too small\n");
        exit(-1);
    }
    printf("    Region is x:%d-%d, y:%d-%d\n",reg->x1, reg->x2, reg->y1, reg->y2);
    return 1;
}


//-----------------------------------------------------------------------------------
// Parse command line switches
// Returns argv[] index of first file-name argument (== argc if none).
//-----------------------------------------------------------------------------------
static int parse_parameter (const char * tag, const char * value)
{
    if (keymatch(tag, "debug", 1) || keymatch(tag, "verbose", 1)) {
        // Enable debug printouts.  Specify more than once for more detail.
        Verbosity += 1;
        return 1;
	}
	if (!value){
        fprintf(stderr, "Parameter '%s' needs to be followed by a vaue\n",tag);
        return -1;
	}

    if (keymatch(tag, "spurious", 4)) {
        SpuriousReject = value[0]-'0';
		if ((SpuriousReject != 0 && SpuriousReject != 1) || value[1] != 0){
			fprintf(stderr, "Spurious value can only be 0 or 1\n");
		}
    }else if (keymatch(tag, "postmotion", 11)) {
        if (sscanf(value, "%d", &PostMotionKeep) != 1) return -1;
    }else if (keymatch(tag, "sendsigs", 8)) {
        if (sscanf(value, "%d", &SendTriggerSignals) != 1) return -1;        
    }else if (keymatch(tag, "brmonitor", 5)) {
        if (sscanf(value, "%d", &BrightnessChangeRestart) != 1) return -1;        
    } else if (keymatch(tag, "scale", 2)) {
        // Scale the output image by a fraction 1/N.
        if (sscanf(value, "%d", &ScaleDenom) != 1) return -1;
    } else if (keymatch(tag, "sensitivity", 2)) {
        // Sensitivity level (lower = more senstitive) 
        if (sscanf(value, "%d", &Sensitivity) != 1) return -1;
    } else if (keymatch(tag, "timelapse", 4)) {
        // Scale the output image by a fraction M/N.
        if (sscanf(value, "%d", (int *)&TimelapseInterval) != 1) return -1;
        if (TimelapseInterval < 1){
            fprintf(stderr,"timelapse interval must be at least 1 second\n");
            return -1;
        }
    } else if (keymatch(tag, "aquire_cmd", 4)) {
        // Set output file name.
        strncpy(raspistill_cmd, value, sizeof(raspistill_cmd)-1);
    } else if (keymatch(tag, "blink_cmd", 5)) {
        // Set output file name.
        strncpy(blink_cmd, value, sizeof(blink_cmd)-1);
    } else if (keymatch(tag, "savedir", 4)) {
        // Set output file name.
        strncpy(SaveDir,value, sizeof(SaveDir)-1);
    } else if (keymatch(tag, "savenames", 5)) {
        // Set output file name.
        strncpy(SaveNames,value, sizeof(SaveNames)-1);
        {
            int a,b;
            for (a=0;SaveNames[a];a++){
                if (SaveNames[a] == '%'){
                    for (b=0;b<10;b++) if (SaveNames[a+1] == "dHjmMSUwyY"[b]) break;
                    if (b >=10){
                        fprintf(stderr, "savenames '%%' may only be followed by one of d,H,j,m,M,S,U,w,y, or Y\n");
                        exit(-1);
                    }
                }
            }
        }

    } else if (keymatch(tag, "logtofile", 8)) {
        // Log to a file instead of stdout
        strncpy(LogToFile,value, sizeof(SaveDir)-1);
    } else if (keymatch(tag, "movelognames", 12)) {
        // Log to a file instead of stdout
        strncpy(MoveLogNames,value, sizeof(SaveDir)-1);

    } else if (keymatch(tag, "region", 3)) {
        if (!ParseRegion(&Regions.DetectReg, value)) goto bad_value;
//    } else if (keymatch(tag, "mouseregion", 11)) {
//        if (!ParseRegion(&MouseDetectRegion, value)) goto bad_value;
    } else if (keymatch(tag, "exclude", 4)) {
        if (Regions.NumExcludeReg >= MAX_EXCLUDE_REGIONS){
            fprintf(stderr, "too many exclude regions");
            exit(-1);
        }else{
            Region_t NewEx;
            if (!ParseRegion(&NewEx, value)) goto bad_value;
            printf("Exclude region x:%d-%d, y:%d-%d\n",NewEx.x1, NewEx.x2, NewEx.y1, NewEx.y2);
            Regions.ExcludeReg[Regions.NumExcludeReg++] = NewEx;
        }
    } else if (keymatch(tag, "diffmap", 5)) {
        strncpy(DiffMapFileName,value, sizeof(DiffMapFileName)-1);

    } else if (keymatch(tag, "dodir", 5)) {
        // Scale the output image by a fraction M/N. */
        strncpy(DoDirName,value, sizeof(DoDirName)-1);
		FollowDir = 0;
		
    } else if (keymatch(tag, "followdir", 6)) {
        strncpy(DoDirName,value, sizeof(DoDirName)-1);
        FollowDir = 1;
    } else {
        fprintf(stderr,"argument %s not understood\n",tag);
        return -1;	   // bogus switch
        bad_value:
        fprintf(stderr, "Value of %s=%s\n not understood\n",tag,value);
        return -1;

    }
    return 2;
}


//-----------------------------------------------------------------------------------
// Parse command line switches
// Returns argv[] index of first file-name argument (== argc if none).
//-----------------------------------------------------------------------------------
int parse_switches (int argc, char **argv, int last_file_arg_seen)
{
    int argn,a;
    char * arg;
    char * value;

    // Scan command line options, adjust parameters
    for (argn = 1; argn < argc;) {
        //printf("argn = %d\n",argn);
        arg = argv[argn];
        if (*arg != '-') {
            return argn;
        }
        value = NULL;
        if (argn+1 < argc) value = argv[argn+1];

        a = parse_parameter(arg+1, value);
        if (a < 0) usage();
        argn += a;
    }
    return argc;
}

//-----------------------------------------------------------------------------------
// Too many parameters for imgcomp running.  Just read them from a configuration file.
//-----------------------------------------------------------------------------------
void read_config_file()
{
    FILE * file;
    char ConfLine[201];
    int linenum = 0;

    file = fopen("imgcomp.conf", "r");
    if (file == NULL){
        fprintf(stderr, "No configuration file imgcomp.conf\n");
        return;
    }
    for(;;){
        char *s, *value, *t;
        int len, a;
        s = fgets(ConfLine, sizeof(ConfLine)-1, file);
        ConfLine[sizeof(ConfLine)-1] = '\0';
        if (s == NULL) break;
        linenum += 1;

        // Remove leading spaces
        while (*s == ' ' || *s == '\t') s++;

        // Check line length not exceeded.
        len = strlen(s);
        if(len >= sizeof(ConfLine)-1){
            fprintf(stderr, "Configuration line too long:\n%s",s);
            goto no_good;
        }

        // Remove trailing spaces and linefeed.
        t = s+len-1;
        while (*t <= ' ' && t > s) *t-- = '\0';

        if (*s == '#') continue; // comment.
        if (*s == '\r' || *s == '\n') continue; // Blank line.

        value = strstr(s, "=");
        if (value != NULL){
            t = value;

            // Remove value leading spaces
            value += 1;
            while (*value == ' ' || *value == '\t') value++;

            // remove tag trailing spaces.
            *t = '\0';
            t--;
            while (*t == ' ' || *t == '\t'){
                *t = '\0';
                t--;
            }
        }
        
        {
            static int msg_shown = 0;
            if (!msg_shown) printf("Configuration parameters:\n");
            msg_shown = 1;
        }

        // Now finally have the tag extracted.
        printf("    '%s' = '%s'\n",s, value);

        a = parse_parameter(s,value);
        if (a < 0){
no_good:            
            fprintf(stderr, "Error on line %d of imgcomp.conf",linenum);
            exit(-1);
        }

    }
}