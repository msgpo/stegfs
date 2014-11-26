/*
 * stegfs ~ a steganographic file system for unix-like systems
 * Copyright © 2007-2014, albinoloverats ~ Software Development
 * email: stegfs@albinoloverats.net
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <mhash.h>
#include <mcrypt.h>

#include "common/common.h"
#include "common/error.h"
#include "common/tlv.h"

#include "stegfs.h"

#define RATIO 1024

static uint8_t stegfs_root_hash[] = { 0x60, 0xA6, 0x63, 0x2B, \
                                      0x77, 0xB6, 0xD5, 0x78, \
                                      0x9A, 0x65, 0x59, 0x7B, \
                                      0x10, 0x3A, 0x97, 0x2D };

static int64_t open_filesystem(const char * const restrict path, uint64_t *size, bool force, bool recreate)
{
    int64_t fs = 0x0;
    struct stat s;
    memset(&s, 0x00, sizeof s);
    stat(path, &s);
    switch (s.st_mode & S_IFMT)
    {
        case S_IFBLK:
            /*
             * use a device as the file system
             */
            if ((fs = open(path, O_WRONLY | F_WRLCK, S_IRUSR | S_IWUSR)) < 0)
                die("could not open the block device");
            *size = lseek(fs, 0, SEEK_END);
            break;
        case S_IFDIR:
        case S_IFCHR:
        case S_IFLNK:
        case S_IFSOCK:
        case S_IFIFO:
            die("unable to create file system on specified device \"%s\"", path);
        case S_IFREG:
            if (!force && !recreate)
                die("file by that name already exists - use -f to force");
            /*
             * file does exist; use its current size as the desired capacity
             * (unless the user has specified a new size)
             */
            if ((fs = open(path, O_WRONLY | F_WRLCK, S_IRUSR | S_IWUSR)) < 0)
                die("could not open file system %s", path);
            {
                uint64_t z = lseek(fs, 0, SEEK_END);
                if (!z && !*size)
                    die("missing required file system size");
                if (!*size)
                    *size = z;
            }
            break;
        default:
            /*
             * file doesn't exist - good, lets create it...
             */
            if ((fs = open(path, O_WRONLY | F_WRLCK | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) < 0)
                die("could not open file system %s", path);
        break;
    }
    return fs;
}

static uint64_t parse_size(const char * const restrict s)
{
    /*
     * parse the value for our file system size (if using a file on an
     * existing file system) allowing for suffixes: MB, GB, TB, PB and
     * EB - sizes less than 1MB or greater than an Exbibyte are not
     * supported
     */
    char *f;
    uint64_t z = strtol(s, &f, 0);
    switch (toupper(f[0]))
    {
        case 'E':
            z *= RATIO;
        case 'P':
            z *= RATIO;
        case 'T':
            z *= RATIO;
        case 'G':
            z *= RATIO;
        case 'M':
        case '\0':
            z *= MEGABYTE;
            break;
        default:
            die("unknown size suffix %c", f[0]);
    }
    return z;
}

static void adjust_units(double *size, char *units)
{
    if (*size >= RATIO)
    {
        *size /= RATIO;
        strcpy(units, "GB");
    }
    if (*size >= RATIO)
    {
        *size /= RATIO;
        strcpy(units, "TB");
    }
    if (*size >= RATIO)
    {
        *size /= RATIO;
        strcpy(units, "PB");
    }
    if (*size >= RATIO)
    {
        *size /= RATIO;
        strcpy(units, "EB");
    }
    return;
}

static MCRYPT crypto_init(void)
{
    MCRYPT c = mcrypt_module_open(MCRYPT_SERPENT, NULL, MCRYPT_CBC, NULL);
    /* create the initial key for the encryption algorithm */
    uint8_t key[SIZE_BYTE_TIGER] = { 0x00 };
    for (unsigned i = 0; i < sizeof key; i++)
        key[i] = (uint8_t)(lrand48() & 0xFF);
    /* create the initial iv for the encryption algorithm */
    uint8_t iv[SIZE_BYTE_SERPENT] = { 0x00 };
    for (unsigned i = 0; i < sizeof iv; i++)
        iv[i] = (uint8_t)(lrand48() & 0xFF);
    /* done */
    mcrypt_generic_init(c, key, sizeof key, iv);
    return c;
}

static void superblock_info(stegfs_block_t *sb, char *cipher, char *mode, char *hash)
{
    TLV_HANDLE tlv = tlv_init();

    tlv_t t = { TAG_STEGFS, strlen(STEGFS_NAME), (byte_t *)STEGFS_NAME };
    tlv_append(&tlv, t);

    t.tag = TAG_VERSION;
    t.length = strlen(STEGFS_VERSION);
    t.value = (byte_t *)STEGFS_VERSION;
    tlv_append(&tlv, t);

    t.tag = TAG_CIPHER;
    t.length = strlen(cipher);
    t.value = (byte_t *)cipher;
    tlv_append(&tlv, t);

    t.tag = TAG_MODE;
    t.length = strlen(mode);
    t.value = (byte_t *)mode;
    tlv_append(&tlv, t);

    t.tag = TAG_HASH;
    t.length = strlen(hash);
    for (size_t i = 0; i < strlen(hash); i++)
        hash[i] = tolower(hash[i]);
    t.value = (byte_t *)hash;
    tlv_append(&tlv, t);

    memcpy(sb->data, tlv_export(tlv), tlv_size(tlv));

    return;
}

static void print_usage_help(char *arg)
{
    fprintf(stderr, "Usage: %s <-s size> <file system>\n", arg);
    fprintf(stderr, "\n");
    fprintf(stderr, "  -s <size> Desired file system size, required when creating a file system in\n");
    fprintf(stderr, "            a nomal file\n");
    fprintf(stderr, "  -f        Force overwrite existing file, required when overwriting a file\n");
    fprintf(stderr, "            system in a normal file\n");
    fprintf(stderr, "  -r        Rewrite the superblock (perhaps it became corrupt)\n");
    fprintf(stderr, "\nNotes:\n  • Size option isn't necessary when creating a file system on a block device.\n");
    fprintf(stderr, "  • Size are in megabytes unless otherwise told: GB, TB, PB, or EB.\n");
    exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
    if (argc <= 1)
        print_usage_help(argv[0]);

    char *path = NULL;
    uint64_t size = 0;
    bool force = false;
    bool recreate = false;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp("-h", argv[i]))
            print_usage_help(argv[0]);
        else if (!strcmp("-s", argv[i]))
            size = parse_size(argv[(++i)]);
        else if (!strcmp("-f", argv[i]))
            force = true;
        else if (!strcmp("-r", argv[i]))
            recreate = true;
        else
            path = argv[i];
    }

    int64_t fs = open_filesystem(path, &size, force, recreate);

    uint64_t blocks = size / SIZE_BYTE_BLOCK;
    ftruncate(fs, size);

    if (recreate)
        goto superblock;
    /*
     * display some “useful” information about the file system being
     * created
     */
    char s1[9];
    char *s2;
    int l;

    printf("location     : %s\n", path);
    double z = size / MEGABYTE;
    printf("blocks       : %8ju\n", blocks);
    char units[] = "MB";
    adjust_units(&z, units);
    sprintf(s1, "%f", z);
    s2 = strchr(s1, '.');
    l = s2 - s1;
    printf("size         : %8.*g %s\n", (l + 2), z, units);
    z = ((double)size / SIZE_BYTE_BLOCK * SIZE_BYTE_DATA) / MEGABYTE;
    strcpy(units, "MB");
    adjust_units(&z, units);
    sprintf(s1, "%f", z);
    s2 = strchr(s1, '.');
    l = s2 - s1;
    printf("capacity     : %8.*g %s\n", (l + 2), z, units);
    printf("largest file : %8.*g %s\n", (l + 2), z / MAX_COPIES , units);

    /*
     * write “encrypted” blocks
     */
    uint8_t rnd[MEGABYTE];
    MCRYPT mc = crypto_init();
    lseek(fs, 0, SEEK_SET);
    for (uint64_t i = 0; i < size / MEGABYTE; i++)
    {
        printf("\rwriting      : %8.3f %%", 100.0 * i / (size / MEGABYTE));
        mcrypt_generic(mc, rnd, sizeof rnd);
        for (int j = 0; j < 8192; j++)
            memcpy(rnd + j * SIZE_BYTE_BLOCK, stegfs_root_hash, sizeof stegfs_root_hash);
        write(fs, rnd, sizeof rnd);
    }
    printf("\rwriting      : %8.3f %%\n", 100.0);

superblock:
    printf("superblock   : ");
    stegfs_block_t sb;
    memset(sb.path, 0xFF, sizeof sb.path);
    superblock_info(&sb, MCRYPT_SERPENT, MCRYPT_CBC, (char *)mhash_get_hash_name(MHASH_TIGER));
    sb.hash[0] = htonll(MAGIC_0);
    sb.hash[1] = htonll(MAGIC_1);
    sb.hash[2] = htonll(MAGIC_2);
    sb.next[0] = htonll(blocks);
    pwrite(fs, &sb, sizeof sb, 0);
    printf("done\n");
    close(fs);

    return EXIT_SUCCESS;
}
