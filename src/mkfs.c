/*
 * stegfs ~ a steganographic file system for unix-like systems
 * Copyright (c) 2007-2011, albinoloverats ~ Software Development
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
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <time.h>
#include <fcntl.h>
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mcrypt.h>
#include <libintl.h>

#include "common/common.h"
#include "common/list.h"
#include "common/tlv.h"

#include "src/fuse-stegfs.h"
#include "src/lib-stegfs.h"

static uint64_t size_in_mb(const char *);

int main(int argc, char **argv)
{
    uint64_t  fs_size = 0x0;
    char     *fs_name = NULL;

    bool frc  = false;
    bool rstr = false;

    if (argc < ARGS_MINIMUM)
    {
        show_usage(USAGE_STRING);
        return EXIT_FAILURE;
    }

    args_t filesystem = {'f', "filesystem", false, true,  NULL, ""};
    args_t size       = {'s', "size",       false, true,  NULL, ""};
    args_t force      = {'x', "force",      false, false, NULL, ""};
    args_t restore    = {'r', "restore",    false, false, NULL, ""};

    list_t *opts = list_create(NULL);
    list_append(&opts, &filesystem);
    list_append(&opts, &size);
    list_append(&opts, &force);
    list_append(&opts, &restore);

    list_t *unknown = init(argv[0], SFS_VERSION, USAGE_STRING, argv, NULL, opts);

    if (filesystem.found)
        fs_name = strdup(filesystem.option);
    if (size.found)
        fs_size = size_in_mb(size.option);
    frc = force.found;
    rstr = restore.found;

    if (!fs_name || !fs_size)
    {
        uint16_t lz = list_size(unknown);
        for (uint16_t i = 0; i < lz; i++)
        {
            char *arg = list_get(unknown, i);
            bool alpha = false;
            for (uint16_t j = 0; j < strlen(arg) - 1; j++)
            {
                if (!isdigit(arg[j]))
                {
                    alpha = true;
                    break;
                }
            }
            if (alpha && !fs_name)
                fs_name = strdup(arg);
            else if (!alpha && !fs_size)
                fs_size = size_in_mb(arg);
        }
    }
    if (!fs_name)
    {
        show_usage(USAGE_STRING);
        return EXIT_FAILURE;
    }
    /*
     * is this a device or a file?
     */
    int64_t fs = 0x0;
    struct stat fs_stat;
    memset(&fs_stat, 0x00, sizeof( fs_stat ));
    stat(fs_name, &fs_stat);
    switch (fs_stat.st_mode & S_IFMT)
    {
        case S_IFBLK:
            /*
             * use a device as the file system
             */
            if ((fs = open(fs_name, O_WRONLY | F_WRLCK, S_IRUSR | S_IWUSR)) < 0)
                die(_("could not open the block device"));
            fs_size = lseek(fs, 0, SEEK_END) / RATIO_BYTE_MB;
            break;
        case S_IFDIR:
        case S_IFCHR:
        case S_IFLNK:
        case S_IFSOCK:
        case S_IFIFO:
            die(_("unable to create file system on specified device \"%s\""), fs_name);
        case S_IFREG:
            if (!frc && !rstr)
                die(_("file by that name already exists - use -x to force"));
            /*
             * file does exist; use its current size as the desired capacity
             * (unless the user has specified a new size)
             */
            if ((fs = open(fs_name, O_WRONLY | F_WRLCK, S_IRUSR | S_IWUSR)) < 0)
                log_message(LOG_ERROR, _("could not open file system %s"), fs_name);
            fs_size = fs_size ? fs_size : (uint64_t)lseek(fs, 0, SEEK_END) / RATIO_BYTE_MB;
            break;
        default:
            /*
             * file doesn't exist - good, lets create it...
             */
            if ((fs = open(fs_name, O_WRONLY | F_WRLCK | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) < 0)
                die(_("could not open file system %s"), fs_name);
        break;
    }
    uint64_t fs_blocks = 0x0;
    if (rstr)
    {
        log_message(LOG_INFO, _("restoring superblock on %s"), fs_name);
        fs_blocks = lseek(fs, 0, SEEK_END) / SIZE_BYTE_BLOCK;
    }
    else
    {
        /*
         * check the size of the file system
         */
        if (fs_size < 1)
            die(_("cannot have a file system with size < 1MB"));
        /*
         * display some information about the soon-to-be file system to the
         * user
         */
        printf(_("location      : %s\n"), fs_name);
        fs_blocks = fs_size * RATIO_BYTE_MB / SIZE_BYTE_BLOCK;
        printf(_("total blocks  : %8ju\n"), fs_blocks);
        {
            char *units = strdup("MB");
            float volume = fs_size;
            if (volume >= RATIO_MB_TB)
            {
                volume /= RATIO_MB_TB;
                units = strdup("TB");
            }
            else if (volume >= RATIO_MB_GB)
            {
                volume /= RATIO_MB_GB;
                units = strdup("GB");
            }
            printf(_("volume        : %8.2f %s\n"), volume, units);
            free(units);
        }
        {
            char *units = strdup("MB");
            float fs_data = fs_size * SIZE_BYTE_DATA;
            fs_data /= SIZE_BYTE_BLOCK;
            if (fs_data >= RATIO_MB_TB)
            {
                fs_data /= RATIO_MB_TB;
                units = strdup("TB");
            }
            else if (fs_data >= RATIO_MB_GB)
            {
                fs_data /= RATIO_MB_GB;
                units = strdup("GB");
            }
            printf(_("data capacity : %8.2f %s\n"), fs_data, units);
            free(units);
        }
        {
            char *units = strdup("MB");
            float fs_avail = fs_size * SIZE_BYTE_DATA;
            fs_avail /= SIZE_BYTE_BLOCK;
            fs_avail /= MAX_COPIES;
            if (fs_avail >= RATIO_MB_TB)
            {
                fs_avail /= RATIO_MB_TB;
                units = strdup("TB");
            }
            else if (fs_avail >= RATIO_MB_GB)
            {
                fs_avail /= RATIO_MB_GB;
                units = strdup("GB");
            }
            printf(_("usable space  : %8.2f %s\n"), fs_avail, units);
            free(units);
        }
        /*
         * create lots of noise
         */
        lseek(fs, 0, SEEK_SET);
        for (uint64_t i = 0; i < fs_size; i++)
        {
            stegfs_block_t buffer[BLOCKS_PER_MB];

            for (uint16_t j = 0; j < BLOCKS_PER_MB; j++)
            {
                uint8_t path[SIZE_BYTE_PATH] =
                {
                    0x1B, 0x9D, 0xCC, 0x02,
                    0xA6, 0xAF, 0x57, 0xA1,
                    0xAC, 0xF2, 0x76, 0x8B,
                    0x29, 0x2B, 0xBE, 0x33
                };
                /*
                 * using 8bit bytes makes using rand48() easier
                 */
                uint8_t data[SIZE_BYTE_DATA];
                uint8_t hash[SIZE_BYTE_HASH];
                uint8_t next[SIZE_BYTE_NEXT];

                for (uint8_t k = 0; k < SIZE_BYTE_DATA; k++)
                    data[k] = mrand48();
                for (uint8_t k = 0; k < SIZE_BYTE_HASH; k++)
                    hash[k] = mrand48();
                for (uint8_t k = 0; k < SIZE_BYTE_NEXT; k++)
                    next[k] = mrand48();

                memcpy(buffer[j].path, path, SIZE_BYTE_PATH);
                memcpy(buffer[j].data, data, SIZE_BYTE_DATA);
                memcpy(buffer[j].hash, hash, SIZE_BYTE_HASH);
                memcpy(buffer[j].next, next, SIZE_BYTE_NEXT);
            }
            if (write(fs, buffer, RATIO_BYTE_MB) != RATIO_BYTE_MB)
                log_message(LOG_ERROR, _("could not create the file system"));
        }
    }
    /*
     * write a 'super' block at the beginning
     */
    {
        stegfs_block_t sb;
        memset(sb.path, 0xFF, SIZE_BYTE_PATH);
        memset(sb.data, 0x00, SIZE_BYTE_DATA);
        /*
         * use TLV format for storing data in SB
         */
        list_t *header = list_create(NULL);
        tlv_t t = tlv_combine(STEGFS_FS_NAME, strlen(PATH_ROOT), PATH_ROOT);
        list_append(&header, &t);
        t = tlv_combine(STEGFS_VERSION, strlen(SFS_VERSION), SFS_VERSION);
        list_append(&header, &t);
        {
            MCRYPT mc = mcrypt_module_open(MCRYPT_SERPENT, NULL, MCRYPT_CBC, NULL);
            char *mca = mcrypt_enc_get_algorithms_name(mc);
            t = tlv_combine(STEGFS_CRYPTO, strlen(mca), mca);
            list_append(&header, &t);
            mcrypt_generic_deinit(mc);
            mcrypt_module_close(mc);
            free(mca);
        }
        {
            char *mha = (char *)mhash_get_hash_name(MHASH_TIGER);
            t = tlv_combine(STEGFS_HASH, strlen(mha), mha);
            list_append(&header, &t);
            free(mha);
        }
        uint8_t *tlv_data = NULL;
        uint64_t tlv_size = tlv_build(&tlv_data, header);
        memcpy(sb.data, tlv_data, tlv_size);

        sb.hash[0] = MAGIC_0;
        sb.hash[1] = MAGIC_1;
        sb.hash[2] = MAGIC_2;

        memcpy(sb.next, &fs_blocks, SIZE_BYTE_NEXT);

        lseek(fs, 0, SEEK_SET);
        if (write(fs, &sb, sizeof( stegfs_block_t )) != sizeof( stegfs_block_t ))
            log_message(LOG_ERROR, _("could not create the file system"));

        list_delete(&header, true);
    }

    close(fs);
    return errno;
}

static uint64_t size_in_mb(const char *s)
{
    /*
     * parse the value for our file system size (if using a file on an
     * existing file system) allowing for suffixes: MB, GB or TB - a
     * size less than 1MB is silly :p and more than 1TB is insane
     */
    char *f;
    uint64_t v = strtol(s, &f, 0);
    switch (toupper(f[0]))
    {
        case 'T':
            v *= RATIO_MB_TB;
            break;
        case 'G':
            v *= RATIO_MB_GB;
            break;
        case 'M':
        case '\0':
            break;
        default:
            die(_("unknown size suffix %c"), f[0]);
    }
    return v;
}

