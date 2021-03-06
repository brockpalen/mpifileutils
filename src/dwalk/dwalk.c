#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h> /* asctime / localtime */

#include <stdarg.h> /* variable length args */

#include <pwd.h> /* for getpwent */
#include <grp.h> /* for getgrent */
#include <errno.h>
#include <assert.h>
#include <inttypes.h>
#include <string.h>
#include <math.h>

#include "libcircle.h"
#include "dtcmp.h"
#include "mfu.h"
#include "mfu_flist.h"

// getpwent getgrent to read user and group entries

/* keep stats during walk */
uint64_t total_dirs    = 0;
uint64_t total_files   = 0;
uint64_t total_links   = 0;
uint64_t total_unknown = 0;
uint64_t total_bytes   = 0;

#define MAX_DISTRIBUTE_SEPARATORS 128
struct distribute_option {
    int separator_number;
    uint64_t separators[MAX_DISTRIBUTE_SEPARATORS];
};

static void create_default_separators(struct distribute_option *option,
                                      mfu_flist* flist,
                                      uint64_t* size,
                                      int* separators,
                                      uint64_t* global_max_file_size)
{
    /* get local max file size for Allreduce */
    uint64_t local_max_file_size = 0;
    for (int i = 0; i < *size; i++) {
        uint64_t file_size = mfu_flist_file_get_size(*flist, i);
        if (file_size > local_max_file_size) {
            local_max_file_size = file_size;
        }
    }

    /* get the max file size across all ranks */
    MPI_Allreduce(&local_max_file_size, global_max_file_size, 1,
                  MPI_UINT64_T, MPI_MAX, MPI_COMM_WORLD);

    /* print and convert max file size to appropriate units */
    double max_size_tmp;
    const char* max_size_units;
    mfu_format_bytes(*global_max_file_size, &max_size_tmp, &max_size_units);
    printf("Max File Size: %.3lf %s\n", max_size_tmp, max_size_units);

    /* round next_pow_2 to next multiple of 10 */
    uint64_t max_magnitude_bin = (ceil(log2(*global_max_file_size) / 10 )) * 10;

    /* get bin ranges based on max file size */
    option->separators[0] = 1;

    /* plus one is for zero count bin */
    *separators = max_magnitude_bin / 10;
    int power = 10;
    for (int i = 1; power <= max_magnitude_bin; i++) {
        option->separators[i] = pow(2, power);
        power+=10;
    }
}

static int print_flist_distribution(int file_histogram,
                                    struct distribute_option *option,
                                    mfu_flist* pflist, int rank)
{
    /* file list to use */
    mfu_flist flist = *pflist;

    /* get local size for each rank, and max file sizes */
    uint64_t size = mfu_flist_size(flist);
    uint64_t global_max_file_size;

    int separators = 0;
    if (file_histogram) {
        /* create default separators */
        create_default_separators(option, &flist, &size, &separators,
                                  &global_max_file_size);
    } else {
        separators = option->separator_number;
    }

    /* allocate a count for each bin, initialize the bin counts to 0
     * it is separator + 1 because the last bin is the last separator
     * to the DISTRIBUTE_MAX */
    uint64_t* dist = (uint64_t*) MFU_MALLOC((separators + 1) * sizeof(uint64_t));

    /* initialize the bin counts to 0 */
    for (int i = 0; i <= separators; i++) {
        dist[i] = 0;
    }

    /* for each file, identify appropriate bin and increment its count */
    for (int i = 0; i < size; i++) {
         /* get the size of the file */
         uint64_t file_size = mfu_flist_file_get_size(flist, i);

         /* loop through the bins and find the one the file belongs to,
          * set last bin to -1, if a bin is not found while looping through the
          * list of file size separators, then it belongs in the last bin
          * so (last file size - MAX bin) */
         int max_bin_flag = -1;
         for (int j = 0; j < separators; j++) {
             if (file_size <= option->separators[j]) {
                 /* found the bin set bin index & increment its count */
                 dist[j]++;

                 /* a file for this bin was found so can't belong to
                  * last bin (so set the flag) & exit the loop */
                 max_bin_flag = 1;
                 break;
             }
         }

         /* if max_bin_flag is still -1 then the file belongs to the last bin */
         if (max_bin_flag < 0) {
             dist[separators]++;
         }
    }

    /* get the total sum across all of the bins */
    uint64_t* disttotal = (uint64_t*) MFU_MALLOC((separators + 1) * sizeof(uint64_t));
    MPI_Allreduce(dist, disttotal, (uint64_t)separators + 1, MPI_UINT64_T, MPI_SUM, MPI_COMM_WORLD);

    /* Print the file distribution */
    if (rank == 0) {
         /* number of files in a bin */
         uint64_t number;
         double size_tmp;
         const char* size_units;
         printf("%-27s %s\n", "Range", "Number");
         for (int i = 0; i <= separators; i++) {
             printf("%s", "[ ");
             if (i == 0) {
                 printf("%7.3lf %2s", 0.000, "B");
             } else {
                 mfu_format_bytes((uint64_t)option->separators[i - 1],
                                  &size_tmp, &size_units);
                 printf("%7.3lf %2s", size_tmp, size_units);
             }

             printf("%s", " - ");

             if (file_histogram) {
                mfu_format_bytes((uint64_t)option->separators[i],
                                 &size_tmp, &size_units);
                number = disttotal[i];
                mfu_format_bytes((uint64_t)option->separators[i],
                                 &size_tmp, &size_units);
                printf("%7.3lf %2s ) %"PRIu64"\n", size_tmp,
                       size_units, number);
             } else {
                if (i == separators) {
                    number = disttotal[i];
                    printf("%10s ) %"PRIu64"\n", "MAX", number);
                } else {
                    number = disttotal[i];
                    mfu_format_bytes((uint64_t)option->separators[i],
                              &size_tmp, &size_units);
                    printf("%7.3lf %2s ) %"PRIu64"\n", size_tmp, size_units, number);
                }
             }
         }
    }

    /* free the memory used to hold bin counts */
    mfu_free(&disttotal);
    mfu_free(&dist);

    return 0;
}

/* * Search the right position to insert the separator * If the separator exists already, return failure * Otherwise, locate the right position, and move the array forward to
 * save the separator.
 */
static int distribute_separator_add(struct distribute_option *option, uint64_t separator)
{
    int low = 0;
    int high;
    int middle;
    int pos;
    int count;

    count = option->separator_number;
    option->separator_number++;
    if (option->separator_number > MAX_DISTRIBUTE_SEPARATORS) {
        printf("Too many separators");
        return -1;
    }

    if (count == 0) {
        option->separators[0] = separator;
        return 0;
    }

    high = count - 1;
    while (low < high)
    {
        middle = (high - low) / 2 + low;
        if (option->separators[middle] == separator)
            return -1;
        /* In the left half */
        else if (option->separators[middle] < separator)
            low = middle + 1;
        /* In the right half */
        else
            high = middle;
    }
    assert(low == high);
    if (option->separators[low] == separator)
        return -1;

    if (option->separators[low] < separator)
        pos = low + 1;
    else
        pos = low;

    if (pos < count)
        memmove(&option->separators[low + 1], &option->separators[low],
                sizeof(*option->separators) * (uint64_t)(count - pos));

    option->separators[pos] = separator;
    return 0;
}

static int distribution_parse(struct distribute_option *option, const char *string)
{
    char *ptr;
    char *next;
    unsigned long long separator;
    char *str;
    int status = 0;

    if (strncmp(string, "size", strlen("size")) != 0) {
        return -1;
    }

    option->separator_number = 0;
    if (strlen(string) == strlen("size")) {
        return 0;
    }

    if (string[strlen("size")] != ':') {
        return -1;
    }

    str = MFU_STRDUP(string);
    /* Parse separators */
    ptr = str + strlen("size:");
    next = ptr;
    while (ptr && ptr < str + strlen(string)) {
        next = strchr(ptr, ',');
        if (next != NULL) {
            *next = '\0';
            next++;
        }

        if (mfu_abtoull(ptr, &separator) != MFU_SUCCESS) {
            printf("Invalid separator \"%s\"\n", ptr);
            status = -1;
            goto out;
        }

        if (distribute_separator_add(option, separator)) {
            printf("Duplicated separator \"%llu\"\n", separator);
            status = -1;
            goto out;
        }

        ptr = next;
    }

out:
    mfu_free(&str);
    return status;
}

static void print_usage(void)
{
    printf("\n");
    printf("Usage: dwalk [options] <path> ...\n");
    printf("\n");
    printf("Options:\n");
    printf("  -i, --input <file>      - read list from file\n");
    printf("  -o, --output <file>     - write processed list to file in binary format\n");
    printf("  --text-output <file>    - write processed list to file in ascii format\n");
    printf("  -l, --lite              - walk file system without stat\n");
    printf("  -s, --sort <fields>     - sort output by comma-delimited fields\n");
    printf("  -d, --distribution <field>:<separators> \n                          - print distribution by field\n");
    printf("  -f, --file_histogram    - print default size distribution of items\n");
    printf("  -p, --print             - print files to screen\n");
    printf("      --progress <N>      - print progress every N seconds\n");
    printf("  -v, --verbose           - verbose output\n");
    printf("  -q, --quiet             - quiet output\n");
    printf("  -h, --help              - print usage\n");
    printf("\n");
    printf("Fields: name,user,group,uid,gid,atime,mtime,ctime,size\n");
    printf("\n");
    printf("Filters:\n");
    printf("  --amin N       - last accessed N minutes ago\n");
    printf("  --anewer FILE  - last accessed more recently than FILE modified\n");
    printf("  --atime N      - last accessed N days ago\n");
    printf("  --cmin N       - status last changed N minutes ago\n");
    printf("  --cnewer FILE  - status last changed more recently than FILE modified\n");
    printf("  --ctime N      - status last changed N days ago\n");
    printf("  --mmin N       - data last modified N minutes ago\n");
    printf("  --mtime N      - data last modified N days ago\n");
    printf("\n");
    printf("  --gid N        - numeric group ID is N\n");
    printf("  --group NAME   - belongs to group NAME\n");
    printf("  --uid N        - numeric user ID is N\n");
    printf("  --user NAME    - owned by user NAME\n");
    printf("\n");
    printf("  --size N       - size is N bytes.  Supports attached units like KB, MB, GB\n");
    printf("  --type C       - of type C: d=dir, f=file, l=symlink\n");
    printf("\n");
    printf("For more information see https://mpifileutils.readthedocs.io. \n");
    printf("\n");
    fflush(stdout);
    return;
}


/* look up mtimes for specified file,
 * return secs/nsecs in newly allocated mfu_pred_times struct,
 * return NULL on error */
static mfu_pred_times* get_mtimes(const char* file)
{
    mfu_param_path param_path;
    mfu_param_path_set(file, &param_path);
    if (! param_path.path_stat_valid) {
        return NULL;
    }
    mfu_pred_times* t = (mfu_pred_times*) MFU_MALLOC(sizeof(mfu_pred_times));
    mfu_stat_get_mtimes(&param_path.path_stat, &t->secs, &t->nsecs);
    mfu_param_path_free(&param_path);
    return t;
}

static int add_type(mfu_pred* p, char t)
{
    mode_t* type = (mode_t*) MFU_MALLOC(sizeof(mode_t));
    switch (t) {
    case 'b':
        *type = S_IFBLK;
        break;
    case 'c':
        *type = S_IFCHR;
        break;
    case 'd':
        *type = S_IFDIR;
        break;
    case 'f':
        *type = S_IFREG;
        break;
    case 'l':
        *type = S_IFLNK;
        break;
    case 'p':
        *type = S_IFIFO;
        break;
    case 's':
        *type = S_IFSOCK;
        break;

    default:
        /* unsupported type character */
        mfu_free(&type);
        return -1;
        break;
    }

    /* add check for this type */
    mfu_pred_add(p, MFU_PRED_TYPE, (void *)type);
    return 1;
}

static void pred_commit (mfu_pred* p)
{
    int need_print = 1;

    mfu_pred* cur = p;
    while (cur) {
//        if (cur->f == MFU_PRED_PRINT || cur->f == MFU_PRED_EXEC) {
//            need_print = 0;
//            break;
//        }
        cur = cur->next;
    }

    if (need_print) {
//        mfu_pred_add(p, MFU_PRED_PRINT, NULL);
    }
}

int main(int argc, char** argv)
{
    int i;

    /* initialize MPI */
    MPI_Init(&argc, &argv);
    mfu_init();

    /* get our rank and the size of comm_world */
    int rank, ranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);

    /* pointer to mfu_walk_opts */
    mfu_walk_opts_t* walk_opts = mfu_walk_opts_new();

    /* capture current time for any time based queries,
     * to get a consistent value, capture and bcast from rank 0 */
    mfu_pred_times* now_t = mfu_pred_now();

    /* TODO: extend options
     *   - allow user to cache scan result in file
     *   - allow user to load cached scan as input
     *
     *   - allow user to filter by user, group, or filename using keyword or regex
     *   - allow user to specify time window
     *   - allow user to specify file sizes
     *
     *   - allow user to sort by different fields
     *   - allow user to group output (sum all bytes, group by user) */

    mfu_pred* pred_head = mfu_pred_new();
    char* inputname      = NULL;
    char* outputname     = NULL;
    char* textoutputname     = NULL;
    char* sortfields     = NULL;
    char* distribution   = NULL;

    int file_histogram       = 0;
    int walk                 = 0;
    int print                = 0;
    int text                 = 0;

    struct distribute_option option;

    /* verbose by default */
    mfu_debug_level = MFU_LOG_VERBOSE;

    int option_index = 0;
    static struct option long_options[] = {
        {"input",          1, 0, 'i'},
        {"output",         1, 0, 'o'},
        {"text-output",     required_argument, NULL, 'z' },
        {"lite",           0, 0, 'l'},
        {"sort",           1, 0, 's'},
        {"distribution",   1, 0, 'd'},
        {"file_histogram", 0, 0, 'f'},
        {"print",          0, 0, 'p'},
        {"progress",       1, 0, 'P'},
        {"verbose",        0, 0, 'v'},
        {"quiet",          0, 0, 'q'},
        {"help",           0, 0, 'h'},

        { "amin",     required_argument, NULL, 'a' },
        { "anewer",   required_argument, NULL, 'B' },
        { "atime",    required_argument, NULL, 'A' },
        { "cmin",     required_argument, NULL, 'c' },
        { "cnewer",   required_argument, NULL, 'D' },
        { "ctime",    required_argument, NULL, 'C' },
        { "mmin",     required_argument, NULL, 'm' },
        { "mtime",    required_argument, NULL, 'M' },

        { "gid",      required_argument, NULL, 'g' },
        { "group",    required_argument, NULL, 'G' },
        { "uid",      required_argument, NULL, 'u' },
        { "user",     required_argument, NULL, 'U' },

        { "size",     required_argument, NULL, 'S' },
        { "type",     required_argument, NULL, 'T' },

        {0, 0, 0, 0}
    };

    int usage = 0;
    while (1) {
        int c = getopt_long(
                    argc, argv, "i:o:tls:d:fpvqh",
                    long_options, &option_index
                );

        if (c == -1) {
            break;
        }

        char* buf;
        mfu_pred_times* t;
        mfu_pred_times_rel* tr;
        int ret;

        switch (c) {
            case 'i':
                inputname = MFU_STRDUP(optarg);
                break;
            case 'o':
                outputname = MFU_STRDUP(optarg);
                break;
            case 'z':
                textoutputname = MFU_STRDUP(optarg);
                break;
            case 'l':
                /* don't stat each file on the walk */
                walk_opts->use_stat = 0;
                break;
            case 's':
                sortfields = MFU_STRDUP(optarg);
                break;
            case 'd':
                distribution = MFU_STRDUP(optarg);
                break;
            case 'f':
                file_histogram = 1;
                break;
            case 'p':
                print = 1;
                break;
            case 'P':
                mfu_progress_timeout = atoi(optarg);
                break;

            case 'a':
                tr = mfu_pred_relative(optarg, now_t);
       	        mfu_pred_add(pred_head, MFU_PRED_AMIN, (void *)tr);
                break;
            case 'm':
                tr = mfu_pred_relative(optarg, now_t);
                mfu_pred_add(pred_head, MFU_PRED_MMIN, (void *)tr);
                break;
            case 'c':
                tr = mfu_pred_relative(optarg, now_t);
                mfu_pred_add(pred_head, MFU_PRED_CMIN, (void *)tr);
                break;

            case 'A':
                tr = mfu_pred_relative(optarg, now_t);
                mfu_pred_add(pred_head, MFU_PRED_ATIME, (void *)tr);
                break;
            case 'M':
                tr = mfu_pred_relative(optarg, now_t);
                mfu_pred_add(pred_head, MFU_PRED_MTIME, (void *)tr);
                break;
            case 'C':
                tr = mfu_pred_relative(optarg, now_t);
                mfu_pred_add(pred_head, MFU_PRED_CTIME, (void *)tr);
                break;

            case 'B':
                    t = get_mtimes(optarg);
                    if (t == NULL) {
                        if (rank == 0) {
                            printf("%s: can't find file %s\n", argv[0], optarg);
                        }
                   exit(1);
                }
                mfu_pred_add(pred_head, MFU_PRED_ANEWER, (void *)t);
                break;
            case 'D':
                t = get_mtimes(optarg);
                if (t == NULL) {
                    if (rank == 0) {
                       printf("%s: can't find file %s\n", argv[0], optarg);
                    }
                exit(1);
                }
                mfu_pred_add(pred_head, MFU_PRED_CNEWER, (void *)t);
                break;

            case 'g':
                /* TODO: error check argument */
                buf = MFU_STRDUP(optarg);
                mfu_pred_add(pred_head, MFU_PRED_GID, (void *)buf);
                break;
        
            case 'G':
                buf = MFU_STRDUP(optarg);
                mfu_pred_add(pred_head, MFU_PRED_GROUP, (void *)buf);
                break;
        
            case 'u':
                /* TODO: error check argument */
                buf = MFU_STRDUP(optarg);
                mfu_pred_add(pred_head, MFU_PRED_UID, (void *)buf);
                break;
        
            case 'U':
                buf = MFU_STRDUP(optarg);
                mfu_pred_add(pred_head, MFU_PRED_USER, (void *)buf);
                break;

            case 'S':
                buf = MFU_STRDUP(optarg);
                mfu_pred_add(pred_head, MFU_PRED_SIZE, (void *)buf);
                break;

            case 'T':
                ret = add_type(pred_head, *optarg);
                if (ret != 1) {
                    if (rank == 0) {
                    printf("%s: unsupported file type %s\n", argv[0], optarg);
                    }
                exit(1);
                }
                break;
        
            case 'v':
                mfu_debug_level = MFU_LOG_VERBOSE;
                break;
            case 'q':
                mfu_debug_level = 0;
                break;
            case 'h':
                usage = 1;
                break;
            case '?':
                usage = 1;
                break;
            default:
                if (rank == 0) {
                    printf("?? getopt returned character code 0%o ??\n", c);
                }
        }
    }

    pred_commit(pred_head);

    /* check that we got a valid progress value */
    if (mfu_progress_timeout < 0) {
        if (rank == 0) {
            MFU_LOG(MFU_LOG_ERR, "Seconds in --progress must be non-negative: %d invalid", mfu_progress_timeout);
        }
        usage = 1;
    }

    /* paths to walk come after the options */
    int numpaths = 0;
    mfu_param_path* paths = NULL;
    if (optind < argc) {
        /* got a path to walk */
        walk = 1;

        /* determine number of paths specified by user */
        numpaths = argc - optind;

        /* allocate space for each path */
        paths = (mfu_param_path*) MFU_MALLOC((size_t)numpaths * sizeof(mfu_param_path));

        /* process each path */
        char** p = &argv[optind];
        mfu_param_path_set_all((uint64_t)numpaths, (const char**)p, paths);
        optind += numpaths;

        /* don't allow user to specify input file with walk */
        if (inputname != NULL) {
            usage = 1;
        }
    }
    else {
        /* if we're not walking, we must be reading,
         * and for that we need a file */
        if (inputname == NULL) {
            usage = 1;
        }
    }

    /* at least one filter was applied which requires stat */
    if (walk_opts->use_stat == 0 && pred_head->next != NULL) {
        if (rank == 0) {
            printf("Filters (atime, mtime, etc.) requires stat\n");
        }
        usage = 1;
    }

    /* if user is trying to sort, verify the sort fields are valid */
    if (sortfields != NULL) {
        int maxfields;
        int nfields = 0;
        char* sortfields_copy = MFU_STRDUP(sortfields);
        if (walk_opts->use_stat) {
            maxfields = 7;
            char* token = strtok(sortfields_copy, ",");
            while (token != NULL) {
                if (strcmp(token,  "name")  != 0 &&
                        strcmp(token, "-name")  != 0 &&
                        strcmp(token,  "user")  != 0 &&
                        strcmp(token, "-user")  != 0 &&
                        strcmp(token,  "group") != 0 &&
                        strcmp(token, "-group") != 0 &&
                        strcmp(token,  "uid")   != 0 &&
                        strcmp(token, "-uid")   != 0 &&
                        strcmp(token,  "gid")   != 0 &&
                        strcmp(token, "-gid")   != 0 &&
                        strcmp(token,  "atime") != 0 &&
                        strcmp(token, "-atime") != 0 &&
                        strcmp(token,  "mtime") != 0 &&
                        strcmp(token, "-mtime") != 0 &&
                        strcmp(token,  "ctime") != 0 &&
                        strcmp(token, "-ctime") != 0 &&
                        strcmp(token,  "size")  != 0 &&
                        strcmp(token, "-size")  != 0) {
                    /* invalid token */
                    if (rank == 0) {
                        printf("Invalid sort field: %s\n", token);
                    }
                    usage = 1;
                }
                nfields++;
                token = strtok(NULL, ",");
            }
        }
        else {
            maxfields = 1;
            char* token = strtok(sortfields_copy, ",");
            while (token != NULL) {
                if (strcmp(token,  "name")  != 0 &&
                        strcmp(token, "-name")  != 0) {
                    /* invalid token */
                    if (rank == 0) {
                        printf("Invalid sort field: %s\n", token);
                    }
                    usage = 1;
                }
                nfields++;
                token = strtok(NULL, ",");
            }
        }
        if (nfields > maxfields) {
            if (rank == 0) {
                printf("Exceeded maximum number of sort fields: %d\n", maxfields);
            }
            usage = 1;
        }
        mfu_free(&sortfields_copy);
    }

    if (distribution != NULL) {
        if (distribution_parse(&option, distribution) != 0) {
            if (rank == 0) {
                printf("Invalid distribution argument: %s\n", distribution);
            }
            usage = 1;
        } else if (rank == 0 && option.separator_number != 0) {
            printf("Separators: ");
            for (i = 0; i < option.separator_number; i++) {
                if (i != 0) {
                    printf(", ");
                }
                printf("%"PRIu64, option.separators[i]);
            }
            printf("\n");
        }
    }

    if (usage) {
        if (rank == 0) {
            print_usage();
        }
        MPI_Finalize();
        return 0;
    }

    /* TODO: check stat fields fit within MPI types */
    // if (sizeof(st_uid) > uint64_t) error(); etc...

    /* create an empty file list with default values */
    mfu_flist flist = mfu_flist_new();

    /* create new mfu_file object */
    mfu_file_t* mfu_file = mfu_file_new();

    if (walk) {
        /* walk list of input paths */
        mfu_flist_walk_param_paths(numpaths, paths, walk_opts, flist, mfu_file);
    }
    else {
        /* read data from cache file */
        mfu_flist_read_cache(inputname, flist);
    }

    /* TODO: filter files */
    //filter_files(&flist);
    mfu_flist flist2 = mfu_flist_filter_pred(flist, pred_head);

    /* sort files */
    if (sortfields != NULL) {
        /* TODO: don't sort unless all_count > 0 */
        mfu_flist_sort(sortfields, &flist2);
    }

    /* print details for individual files */
    if (print) {
        mfu_flist_print(flist2);
    }

    /* print summary statistics of flist2 */
    mfu_flist_print_summary(flist2);

    /* print distribution if user specified this option */
    if (distribution != NULL || file_histogram) {
        print_flist_distribution(file_histogram, &option, &flist2, rank);
    }

    /* write data to cache file */
    if (outputname != NULL) {
        mfu_flist_write_cache(outputname, flist2);
    }

    /* write text version if also requested 
 *   independent of --output */
    if (textoutputname != NULL) {
        mfu_flist_write_text(textoutputname, flist2);
    }

    /* free off the filtered list */
    mfu_flist_free(&flist2);

    /* free users, groups, and files objects */
    mfu_flist_free(&flist);

    /* free predicate list */
    mfu_pred_free(&pred_head);

    /* free memory allocated for options */
    mfu_free(&distribution);
    mfu_free(&sortfields);
    mfu_free(&outputname);
    mfu_free(&textoutputname);
    mfu_free(&inputname);

    /* free the path parameters */
    mfu_param_path_free_all(numpaths, paths);

    /* free memory allocated to hold params */
    mfu_free(&paths);

    /* free the walk options */
    mfu_walk_opts_delete(&walk_opts);

    /* delete file object */
    mfu_file_delete(&mfu_file);

    /* shut down MPI */
    mfu_finalize();
    MPI_Finalize();

    return 0;
}
