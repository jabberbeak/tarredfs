/*
    Copyright (C) 2016 Fredrik Öhrström

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DEFS_H
#define DEFS_H

#define OK 0
#define ERR 1

#define DEFAULT_TARGET_TAR_SIZE 10ull*1024*1024;
#define DEFAULT_TAR_TRIGGER_SIZE 20ull*1024*1024;
#define DEFAULT_SPLIT_TAR_SIZE 100ull*1024*1024;

#define MAX_FILE_NAME_LENGTH 255
#define MAX_PATH_LENGTH 4096
#define MAXPATH 4096
#define ARG_MAX 4096

#ifdef WINAPI
typedef int uid_t;
typedef int gid_t;

/*
enum FileTypes {
    REGTYPE,
    DIRTYPE,
    LNKTYPE,
    SYMTYPE,
    CHRTYPE,
    BLKTYPE,
    FIFTYPE
};

#define S_ISLNK(x) false
#define S_ISREG(x) true
#define S_ISCHR(x) true
#define S_ISBLK(x) true
#define S_ISDIR(x) true
#define S_ISFIFO(x) true

#define S_ISUID 1
#define S_ISGID 2
#define S_ISVTX 4
*/
#endif

#endif
