////////////////////////////////////////////////////////////////////////////////

/*
 * l - an implementation of ls with -a, -l, -i, -r support
 *     along with use of ioctl to determine semi-proper spacing
 *
 *     expanded with:
 *          - safer path handling
 *          - dynamic column formatting like ls
 *          - -1, -F, -h support
 *
 *     written by:
 *         - Jakob Langtry
 */

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

enum
{
    OPT_REVERSE  = 1 << 0,
    OPT_ALL      = 1 << 1,
    OPT_INODE    = 1 << 2,
    OPT_LONG     = 1 << 3,
    OPT_SINGLE   = 1 << 4,
    OPT_CLASSIFY = 1 << 5,
    OPT_HUMAN    = 1 << 6
};

typedef struct
{
    char *name;
    char *path;
    struct stat st;
    char perms[11];
    char *owner;
    char *group;
    char sizebuf[16];
    char timebuf[16];
    size_t display_width;
    int color;
    int bold;
    char suffix;
} Entry;

static void list(const char *target, int optflag, int print_header);
static void print_help(const char *argv0);
static void fill_permissions(const struct stat *src, char dst[11], int *color,
    int *bold);
static char *owner_string(uid_t uid);
static char *group_string(gid_t gid);
static void size_string(const struct stat *src, int optflag, char dst[16]);
static void time_string(const struct stat *src, char dst[16]);
static int winsize(void);
static char *path_join(const char *base, const char *name);
static void free_entries(Entry *entries, int count);
static char suffix_char(const struct stat *src);
static size_t digit_count(unsigned long long value);
static size_t entry_width(const Entry *entry, int optflag);
static void print_name(const Entry *entry, int optflag);
static void print_single_column(const Entry *entries, int count, int optflag);
static void print_long_format(const Entry *entries, int count, int optflag);
static void print_columns(const Entry *entries, int count, int optflag,
    int termwidth);

int main(int argc, char **argv)
{
    int optflag = 0;
    int opt = -1;
    int targets = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-?") == 0)
        {
            print_help(argv[0]);
            return EXIT_SUCCESS;
        }
    }

    while ((opt = getopt(argc, argv, "1railFh")) != -1)
    {
        switch (opt)
        {
            case '1':
                optflag |= OPT_SINGLE;
                break;
            case 'r':
                optflag |= OPT_REVERSE;
                break;
            case 'a':
                optflag |= OPT_ALL;
                break;
            case 'i':
                optflag |= OPT_INODE;
                break;
            case 'l':
                optflag |= OPT_LONG;
                break;
            case 'F':
                optflag |= OPT_CLASSIFY;
                break;
            case 'h':
                optflag |= OPT_HUMAN;
                break;
            default:
                print_help(argv[0]);
                return EXIT_FAILURE;
        }
    }

    targets = argc - optind;
    if (targets > 0)
    {
        for (; optind < argc; optind++)
        {
            list(argv[optind], optflag, targets > 1);
            if (optind < argc - 1)
            {
                printf("\n");
            }
        }
    }
    else
    {
        list(".", optflag, 0);
    }

    return EXIT_SUCCESS;
}

static void list(const char *target, int optflag, int print_header)
{
    struct dirent **dirbuf = NULL;
    struct stat target_stat;
    char *resolved = NULL;
    Entry *entries = NULL;
    int n = 0;
    int count = 0;
    int termwidth = winsize();

    if (lstat(target, &target_stat) != 0)
    {
        perror(target);
        return;
    }

    if (print_header)
    {
        printf("\033[00;37m%s:\n", target);
    }

    if (!S_ISDIR(target_stat.st_mode))
    {
        entries = calloc(1, sizeof(*entries));
        if (!entries)
        {
            perror("calloc");
            exit(EXIT_FAILURE);
        }

        entries[0].name = strdup(target);
        entries[0].path = strdup(target);
        entries[0].st = target_stat;
        entries[0].owner = owner_string(target_stat.st_uid);
        entries[0].group = group_string(target_stat.st_gid);
        fill_permissions(&target_stat, entries[0].perms, &entries[0].color,
            &entries[0].bold);
        entries[0].suffix = (optflag & OPT_CLASSIFY)
            ? suffix_char(&target_stat) : '\0';
        size_string(&target_stat, optflag, entries[0].sizebuf);
        time_string(&target_stat, entries[0].timebuf);
        entries[0].display_width = entry_width(&entries[0], optflag);

        if (optflag & OPT_LONG)
        {
            print_long_format(entries, 1, optflag);
        }
        else if (optflag & OPT_SINGLE)
        {
            print_single_column(entries, 1, optflag);
        }
        else
        {
            print_columns(entries, 1, optflag, termwidth);
        }

        free_entries(entries, 1);
        return;
    }

    resolved = realpath(target, NULL);
    if (!resolved)
    {
        perror("realpath");
        return;
    }

    n = scandir(resolved, &dirbuf, NULL, alphasort);
    if (n < 0)
    {
        perror("scandir");
        free(resolved);
        return;
    }

    entries = calloc((size_t)n, sizeof(*entries));
    if (!entries)
    {
        perror("calloc");
        free(resolved);
        while (n > 0)
        {
            free(dirbuf[--n]);
        }
        free(dirbuf);
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < n; i++)
    {
        Entry *entry = NULL;

        if (!(optflag & OPT_ALL) && dirbuf[i]->d_name[0] == '.')
        {
            free(dirbuf[i]);
            continue;
        }

        entry = &entries[count];
        entry->name = strdup(dirbuf[i]->d_name);
        entry->path = path_join(resolved, dirbuf[i]->d_name);
        if (!entry->name || !entry->path)
        {
            perror("strdup");
            free(dirbuf[i]);
            free(resolved);
            free(dirbuf);
            free_entries(entries, count + 1);
            exit(EXIT_FAILURE);
        }

        if (lstat(entry->path, &entry->st) != 0)
        {
            perror(entry->path);
            free(dirbuf[i]);
            continue;
        }

        entry->owner = owner_string(entry->st.st_uid);
        entry->group = group_string(entry->st.st_gid);
        fill_permissions(&entry->st, entry->perms, &entry->color, &entry->bold);
        entry->suffix = (optflag & OPT_CLASSIFY)
            ? suffix_char(&entry->st) : '\0';
        size_string(&entry->st, optflag, entry->sizebuf);
        time_string(&entry->st, entry->timebuf);
        entry->display_width = entry_width(entry, optflag);

        count++;
        free(dirbuf[i]);
    }

    free(dirbuf);
    free(resolved);

    if ((optflag & OPT_REVERSE) && count > 1)
    {
        for (int left = 0, right = count - 1; left < right; left++, right--)
        {
            Entry temp = entries[left];
            entries[left] = entries[right];
            entries[right] = temp;
        }
    }

    if (optflag & OPT_LONG)
    {
        print_long_format(entries, count, optflag);
    }
    else if (optflag & OPT_SINGLE)
    {
        print_single_column(entries, count, optflag);
    }
    else
    {
        print_columns(entries, count, optflag, termwidth);
    }

    free_entries(entries, count);
}

static void print_help(const char *argv0)
{
    printf("usage: %s [-1Failhr] [file ...]\n", argv0);
    printf("\n");
    printf("options:\n");
    printf("  -1        force one entry per line\n");
    printf("  -a        include hidden files and directories\n");
    printf("  -F        append file type indicators\n");
    printf("  -h        show human-readable sizes in long format\n");
    printf("  -i        display inode numbers\n");
    printf("  -l        use long listing format\n");
    printf("  -r        reverse sort order\n");
    printf("  -?, --help  show this help text\n");
    printf("\n");
    printf("default behavior:\n");
    printf("  `l` prints entries in width-based columns.\n");
    printf("  `l -1` forces single-column output.\n");
    printf("  `l -l` switches to long format.\n");
}

static void fill_permissions(const struct stat *src, char dst[11], int *color,
    int *bold)
{
    mode_t mode = src->st_mode;

    strcpy(dst, "----------");
    *color = 7;
    *bold = 0;

    if (S_ISDIR(mode))
    {
        dst[0] = 'd';
        *color = 4;
    }
    else if (S_ISLNK(mode))
    {
        dst[0] = 'l';
        *color = 6;
    }
    else if (S_ISCHR(mode))
    {
        dst[0] = 'c';
        *color = 3;
    }
    else if (S_ISBLK(mode))
    {
        dst[0] = 'b';
        *color = 3;
    }
    else if (S_ISFIFO(mode))
    {
        dst[0] = 'p';
        *color = 3;
    }
    else if (S_ISSOCK(mode))
    {
        dst[0] = 's';
        *color = 5;
    }

    if (mode & S_IRUSR)
        dst[1] = 'r';
    if (mode & S_IWUSR)
        dst[2] = 'w';
    if (mode & S_IXUSR)
        dst[3] = 'x';
    if (mode & S_IRGRP)
        dst[4] = 'r';
    if (mode & S_IWGRP)
        dst[5] = 'w';
    if (mode & S_IXGRP)
        dst[6] = 'x';
    if (mode & S_IROTH)
        dst[7] = 'r';
    if (mode & S_IWOTH)
        dst[8] = 'w';
    if (mode & S_IXOTH)
        dst[9] = 'x';

    if (mode & S_ISUID)
        dst[3] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID)
        dst[6] = (mode & S_IXGRP) ? 's' : 'S';
    if (mode & S_ISVTX)
        dst[9] = (mode & S_IXOTH) ? 't' : 'T';

    if (*color == 7 && (mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
    {
        *color = 2;
    }
    if (*color != 7)
    {
        *bold = 1;
    }
}

static char *owner_string(uid_t uid)
{
    struct passwd *usrptr = getpwuid(uid);
    char buffer[32];

    if (usrptr)
    {
        return strdup(usrptr->pw_name);
    }

    snprintf(buffer, sizeof(buffer), "%u", (unsigned)uid);
    return strdup(buffer);
}

static char *group_string(gid_t gid)
{
    struct group *grpptr = getgrgid(gid);
    char buffer[32];

    if (grpptr)
    {
        return strdup(grpptr->gr_name);
    }

    snprintf(buffer, sizeof(buffer), "%u", (unsigned)gid);
    return strdup(buffer);
}

static void size_string(const struct stat *src, int optflag, char dst[16])
{
    static const char units[] = "BKMGTPE";
    double size = (double)src->st_size;
    int unit = 0;

    if (!(optflag & OPT_HUMAN))
    {
        snprintf(dst, 16, "%lld", (long long)src->st_size);
        return;
    }

    while (size >= 1024.0 && unit < (int)(sizeof(units) - 1))
    {
        size /= 1024.0;
        unit++;
    }

    if (unit == 0)
    {
        snprintf(dst, 16, "%lldB", (long long)src->st_size);
    }
    else if (size >= 10.0)
    {
        snprintf(dst, 16, "%.0f%c", size, units[unit]);
    }
    else
    {
        snprintf(dst, 16, "%.1f%c", size, units[unit]);
    }
}

static void time_string(const struct stat *src, char dst[16])
{
    time_t now = time(NULL);
    double age = difftime(now, src->st_mtime);
    struct tm *when = localtime(&src->st_mtime);

    if (!when)
    {
        strcpy(dst, "??? ?? ??:??");
        return;
    }

    if (age < 0 || age > 15552000.0)
    {
        strftime(dst, 16, "%b %e  %Y", when);
    }
    else
    {
        strftime(dst, 16, "%b %e %H:%M", when);
    }
}

static int winsize(void)
{
    struct winsize termwidth;
    int termfd = -1;

    if (isatty(STDOUT_FILENO)
        && ioctl(STDOUT_FILENO, TIOCGWINSZ, &termwidth) == 0
        && termwidth.ws_col > 0)
    {
        return termwidth.ws_col;
    }

    termfd = open("/dev/tty", O_RDONLY | O_NOCTTY);
    if (termfd >= 0)
    {
        if (ioctl(termfd, TIOCGWINSZ, &termwidth) == 0 && termwidth.ws_col > 0)
        {
            close(termfd);
            return termwidth.ws_col;
        }
        close(termfd);
    }

    return 80;
}

static char *path_join(const char *base, const char *name)
{
    size_t needs_slash = 0;
    size_t length = 0;
    char *path = NULL;

    if (!base || !name)
    {
        return NULL;
    }

    needs_slash = (base[0] != '\0' && base[strlen(base) - 1] != '/');
    length = strlen(base) + needs_slash + strlen(name) + 1;
    path = malloc(length);
    if (!path)
    {
        return NULL;
    }

    snprintf(path, length, "%s%s%s", base, needs_slash ? "/" : "", name);
    return path;
}

static void free_entries(Entry *entries, int count)
{
    if (!entries)
    {
        return;
    }

    for (int i = 0; i < count; i++)
    {
        free(entries[i].name);
        free(entries[i].path);
        free(entries[i].owner);
        free(entries[i].group);
    }

    free(entries);
}

static char suffix_char(const struct stat *src)
{
    mode_t mode = src->st_mode;

    if (S_ISDIR(mode))
        return '/';
    if (S_ISLNK(mode))
        return '@';
    if (S_ISFIFO(mode))
        return '|';
    if (S_ISSOCK(mode))
        return '=';
    if (mode & (S_IXUSR | S_IXGRP | S_IXOTH))
        return '*';

    return '\0';
}

static size_t digit_count(unsigned long long value)
{
    size_t digits = 1;

    while (value >= 10)
    {
        value /= 10;
        digits++;
    }

    return digits;
}

static size_t entry_width(const Entry *entry, int optflag)
{
    size_t width = strlen(entry->name);

    if (entry->suffix)
    {
        width++;
    }
    if (optflag & OPT_INODE)
    {
        width += digit_count((unsigned long long)entry->st.st_ino) + 1;
    }

    return width;
}

static void print_name(const Entry *entry, int optflag)
{
    if (optflag & OPT_INODE)
    {
        printf("\033[00;37m%lu ", (unsigned long)entry->st.st_ino);
    }

    printf("\033[0%d;3%dm%s", entry->bold, entry->color, entry->name);
    if (entry->suffix)
    {
        printf("%c", entry->suffix);
    }
    printf("\033[00;37m");
}

static void print_single_column(const Entry *entries, int count, int optflag)
{
    for (int i = 0; i < count; i++)
    {
        print_name(&entries[i], optflag);
        printf("\n");
    }
}

static void print_long_format(const Entry *entries, int count, int optflag)
{
    size_t inode_width = 0;
    size_t link_width = 0;
    size_t owner_width = 0;
    size_t group_width = 0;
    size_t size_width = 0;

    for (int i = 0; i < count; i++)
    {
        size_t inode_digits = digit_count((unsigned long long)entries[i].st.st_ino);
        size_t link_digits = digit_count((unsigned long long)entries[i].st.st_nlink);
        size_t owner_digits = strlen(entries[i].owner);
        size_t group_digits = strlen(entries[i].group);
        size_t size_digits = strlen(entries[i].sizebuf);

        if (inode_digits > inode_width)
            inode_width = inode_digits;
        if (link_digits > link_width)
            link_width = link_digits;
        if (owner_digits > owner_width)
            owner_width = owner_digits;
        if (group_digits > group_width)
            group_width = group_digits;
        if (size_digits > size_width)
            size_width = size_digits;
    }

    for (int i = 0; i < count; i++)
    {
        if (optflag & OPT_INODE)
        {
            printf("\033[00;37m%*lu ", (int)inode_width,
                (unsigned long)entries[i].st.st_ino);
        }

        printf("\033[00;37m%s ", entries[i].perms);
        printf("%*lu ", (int)link_width, (unsigned long)entries[i].st.st_nlink);
        printf("%-*s ", (int)owner_width, entries[i].owner);
        printf("%-*s ", (int)group_width, entries[i].group);
        printf("%*s ", (int)size_width, entries[i].sizebuf);
        printf("%s ", entries[i].timebuf);
        print_name(&entries[i], optflag & ~OPT_INODE);
        printf("\n");
    }
}

static void print_columns(const Entry *entries, int count, int optflag,
    int termwidth)
{
    int cols = 1;
    int rows = count;
    int best_cols = 1;
    int *widths = NULL;
    int *best_widths = NULL;

    if (count <= 0)
    {
        return;
    }

    if (termwidth <= 0)
    {
        termwidth = 80;
    }

    for (cols = 1; cols <= count; cols++)
    {
        int candidate_rows = (count + cols - 1) / cols;
        int total_width = 0;

        widths = calloc((size_t)cols, sizeof(*widths));
        if (!widths)
        {
            perror("calloc");
            exit(EXIT_FAILURE);
        }

        for (int col = 0; col < cols; col++)
        {
            for (int row = 0; row < candidate_rows; row++)
            {
                int index = row + col * candidate_rows;

                if (index >= count)
                {
                    break;
                }
                if ((int)entries[index].display_width > widths[col])
                {
                    widths[col] = (int)entries[index].display_width;
                }
            }
            total_width += widths[col];
            if (col < cols - 1)
            {
                total_width += 2;
            }
        }

        if (total_width <= termwidth)
        {
            free(best_widths);
            best_widths = widths;
            best_cols = cols;
            rows = candidate_rows;
            widths = NULL;
        }
        else
        {
            free(widths);
            break;
        }
    }

    if (!best_widths)
    {
        best_widths = calloc(1, sizeof(*best_widths));
        if (!best_widths)
        {
            perror("calloc");
            exit(EXIT_FAILURE);
        }
        best_widths[0] = (int)entries[0].display_width;
    }

    for (int row = 0; row < rows; row++)
    {
        for (int col = 0; col < best_cols; col++)
        {
            int index = row + col * rows;
            int padding = 0;

            if (index >= count)
            {
                continue;
            }

            print_name(&entries[index], optflag);
            if (col < best_cols - 1)
            {
                padding = best_widths[col] - (int)entries[index].display_width + 2;
                while (padding-- > 0)
                {
                    putchar(' ');
                }
            }
        }
        putchar('\n');
    }

    free(best_widths);
}
