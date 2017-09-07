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

#include "tar.h"
#include "tarentry.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <openssl/sha.h>
#include <sstream>
#include <zlib.h>

#include "tarfile.h"
#include "log.h"
#include "util.h"

using namespace std;

static ComponentId TARENTRY = registerLogComponent("tarentry");
static ComponentId HARDLINKS = registerLogComponent("hardlinks");

bool sanityCheck(const char *x, const char *y);

TarEntry::TarEntry(size_t size, TarHeaderStyle ths)
{
    abspath_ = Path::lookupRoot();
    path_ = Path::lookupRoot();
    tar_header_style_ = ths;
    memset(&fs_, 0, sizeof(fs_));
    fs_.st_size = size;
    is_hard_linked_ = false;

    link_ = NULL;
    taz_file_in_use_ = false;
    children_size_ = 0;
    parent_ = NULL;
    is_tar_storage_dir_ = false;
    tarpath_ = path_;
    name_ = Atom::lookup("");

    header_size_ = 0;

    // Round size to nearest 512 byte boundary
    children_size_ = blocked_size_ = (size%T_BLOCKSIZE==0)?size:(size+T_BLOCKSIZE-(size%T_BLOCKSIZE));

    debug(TARENTRY, "Regular File Entry added size %ju blocked size %ju!\n", fs_.st_size, blocked_size_);
}

TarEntry::TarEntry(Path *ap, Path *p, const struct stat *b, TarHeaderStyle ths) : fs_(b)
{
    tar_header_style_ = ths;
    abspath_ = ap;
    path_ = p;
    is_hard_linked_ = false;
    link_ = NULL;
    taz_file_in_use_ = false;
    children_size_ = 0;
    parent_ = NULL;
    is_tar_storage_dir_ = false;
    tarpath_ = path_;
    name_ = p->name();
    
    if (isSymbolicLink())
    {
        char destination[PATH_MAX];
        ssize_t l = readlink(abspath_->c_str(), destination, sizeof(destination));
        if (l < 0) {
            error(TARENTRY, "Could not read link >%s< in underlying filesystem err %d\n",
		  abspath_->c_str(), errno);	   
            return;
        }
        if (l >= PATH_MAX) {
            l = PATH_MAX - 1;
        }
        destination[l] = '\0';
        link_ = Path::lookup(destination);
        debug(TARENTRY, "Found link from %s to %s\n", abspath_->c_str(), destination);
    }

    updateSizes();

    if (tar_header_style_ != TarHeaderStyle::None) {
        stringstream ss;
        ss << permissionString(fs_.st_mode) << separator_string << fs_.st_uid << "/" << fs_.st_gid;
        tv_line_left = ss.str();

        ss.str("");
        if (isSymbolicLink()) {
            ss << 0;
        } else if (isCharacterDevice() || isBlockDevice()) {
            ss << major(fs_.st_rdev) << "," << minor(fs_.st_rdev);
        } else {
            ss << fs_.st_size;
        }
        tv_line_size = ss.str();

        ss.str("");
        char datetime[20];
        memset(datetime, 0, sizeof(datetime));
        strftime(datetime, 20, "%Y-%m-%d %H:%M.%S", localtime(&fs_.st_mtime));
        ss << datetime;
        ss << separator_string;

        char secs_and_nanos[32];
        memset(secs_and_nanos, 0, sizeof(secs_and_nanos));
        snprintf(secs_and_nanos, 32, "%012ju.%09ju", fs_.st_mtim.tv_sec, fs_.st_mtim.tv_nsec);
        ss << secs_and_nanos;
        ss << separator_string;

        memset(secs_and_nanos, 0, sizeof(secs_and_nanos));
        snprintf(secs_and_nanos, 32, "%012ju.%09ju", fs_.st_atim.tv_sec, fs_.st_atim.tv_nsec);
        ss << secs_and_nanos;
        ss << separator_string;

        memset(secs_and_nanos, 0, sizeof(secs_and_nanos));
        snprintf(secs_and_nanos, 32, "%012ju.%09ju", fs_.st_ctim.tv_sec, fs_.st_ctim.tv_nsec);
        ss << secs_and_nanos;

        tv_line_right = ss.str();
    }
    debug(TARENTRY, "Entry %s added\n", path_->c_str());
}

void TarEntry::calculateTarpath(Path *storage_dir) {
    tarpath_ = path_->subpath(storage_dir->depth());
    //tar_.setPath(tarpath_->str());
    //tar_.calculateChecksum();
    tarpath_hash_ = hashString(tarpath_->str());
    //assert(sanityCheck(tarpath_->c_str(), th_get_pathname(tar_)));
}

void TarEntry::createSmallTar(int i) {
    small_tars_[i] = new TarFile(this, SMALL_FILES_TAR, i);
    tars_.push_back(small_tars_[i]);
}
void TarEntry::createMediumTar(int i) {
    medium_tars_[i] = new TarFile(this, MEDIUM_FILES_TAR, i);
    tars_.push_back(medium_tars_[i]);
}
void TarEntry::createLargeTar(uint32_t hash) {
    large_tars_[hash] = new TarFile(this, SINGLE_LARGE_FILE_TAR, hash);
    tars_.push_back(large_tars_[hash]);
}

size_t TarEntry::copy(char *buf, size_t size, size_t from) {
    size_t copied = 0;
    debug(TARENTRY, "Copying from %s\n", name_->c_str());

    if (size > 0 && from < header_size_) {
        debug(TARENTRY, "Copying max %zu from %zu, now inside header (header size=%ju)\n", size, from,
              header_size_);

        char tmp[header_size_];
        memset(tmp, 0, header_size_);
        int p = 0;

	if (is_hard_linked_) {
            // TODO?
	}
	TarHeader th(&fs_, tarpath_, link_, is_hard_linked_, tar_header_style_ == TarHeaderStyle::Full);
	
        if (th.numLongLinkBlocks() > 0)
	{
	    TarHeader llh;
            llh.setLongLinkType(&th);
	    llh.setSize(link_->c_str_len());
            llh.calculateChecksum();

	    memcpy(tmp+p, llh.buf(), T_BLOCKSIZE);
            memcpy(tmp+p+T_BLOCKSIZE, link_->c_str(), link_->c_str_len());
            p += th.numLongLinkBlocks()*T_BLOCKSIZE;
            debug(TARENTRY, "Wrote long link header for %s\n", link_->c_str());
        }

        if (th.numLongPathBlocks() > 0)
	{
	    TarHeader lph;
            lph.setLongPathType(&th);
	    lph.setSize(tarpath_->c_str_len()+1);
            lph.calculateChecksum();

	    memcpy(tmp+p, lph.buf(), T_BLOCKSIZE);
            memcpy(tmp+p+T_BLOCKSIZE, tarpath_->c_str(), tarpath_->c_str_len());
            p += th.numLongPathBlocks()*T_BLOCKSIZE;
            debug(TARENTRY, "Wrote long path header for %s\n", tarpath_->c_str());
        }

        memcpy(tmp+p, th.buf(), T_BLOCKSIZE);

	if (is_hard_linked_) {
	    debug(HARDLINKS, "Copying hard link header out! %s\n", path_->c_str())
	}
	
        // Copy the header out
        size_t len = header_size_-from;
        if (len > size) {
            len = size;
        }
        debug(TARENTRY, "    header out from %s %zu size=%zu\n", path_->c_str(), from, len);
        assert(from+len <= header_size_);
        memcpy(buf, tmp+from, len);
        size -= len;
        buf += len;
        copied += len;
        from += len;
    }

    if (size > 0 && copied < blocked_size_ && from >= header_size_ && from < blocked_size_) {
        debug(TARENTRY, "Copying max %zu from %zu from content %s\n"
	      "with blocked_size=%zu header_size=%zu hard?=%d\n", size, from, tarpath_->c_str(), blocked_size_, header_size_,
	    is_hard_linked_);
        if (virtual_file_) {
            debug(TARENTRY, "Reading from virtual file size=%ju copied=%ju blocked_size=%ju from=%ju header_size=%ju\n",
                  size, copied, blocked_size_, from, header_size_);
            size_t off = from - header_size_;
            size_t len = content.size()-off;
            if (len > size) {
                len = size;
            }
            memcpy(buf, &content[0]+off, len);
            size -= len;
            buf += len;
            copied += len;
        } else {
            debug(TARENTRY, "Reading from file size=%ju copied=%ju blocked_size=%ju from=%ju header_size=%ju\n",
                  size, copied, blocked_size_, from, header_size_);
            // Read from file
            int fd = open(abspath_->c_str(), O_RDONLY);
            if (fd==-1) {
                failure(TARENTRY, "Could not open file >%s< in underlying filesystem err %d", path_->c_str(), errno);
                return 0;
            }
            debug(TARENTRY, "    contents out from %s %zu size=%zu\n", path_->c_str(), from-header_size_, size);
            ssize_t l = pread(fd, buf, size, from-header_size_);
            if (l==-1) {
                failure(TARENTRY, "Could not read from file >%s< in underlying filesystem err %d", path_->c_str(), errno);
                return 0;
            }
            close(fd);
            size -= l;
            buf += l;
            copied += l;
        }
    }
    // Round up to next 512 byte boundary.
    size_t remainder = (copied%T_BLOCKSIZE == 0) ? 0 : T_BLOCKSIZE-copied%T_BLOCKSIZE;
    if (remainder > size) {
        remainder = size;
    }
    memset(buf, 0, remainder);
    copied += remainder;
    debug(TARENTRY, "Copied %zu bytes\n", copied);
    return copied;
}

bool sanityCheck(const char *x, const char *y) {
    if (strcmp(x,y)) {
        if (x[0] == 0 && y[0] == '.' && y[1] == 0) {
            // OK
            return true;
        } else {
            // Something differs ok or not?
            size_t yl = strlen(y);
            if (x[0] == '/' && y[0] != '/') {
                // Skip initial root / that is never stored in tar.
                x++;
            }
            if (yl-1 == strlen(x) && y[yl-1] == '/' && x[yl-1] == 0) {
                // Skip final / after dirs in tar file
                yl--;
            }
            if (strncmp(x,y,yl)) {
                error(TARENTRY, "Internal error, these should be equal!\n>%s<\n>%s<\nlen %zu\n ", x, y, yl);
                return false;
            }
        }
    }
    return true;
}

void TarEntry::setContent(vector<unsigned char> &c) {
    content = c;
    virtual_file_ = true;
    assert((size_t)fs_.st_size == c.size());
}

void TarEntry::updateSizes() {
    size_t size = header_size_ = TarHeader::calculateSize(&fs_, tarpath_, link_, is_hard_linked_);
    if (tar_header_style_ == TarHeaderStyle::None) {
        size = header_size_ = 0;
    }
    if (isRegularFile() && !is_hard_linked_) {
        // Directories, symbolic links and fifos have no content in the tar.
        // Only add the size from actual files with content here.
        size += fs_.st_size;
    }
    // Round size to nearest 512 byte boundary
    children_size_ = blocked_size_ = (size%T_BLOCKSIZE==0)?size:(size+T_BLOCKSIZE-(size%T_BLOCKSIZE));

    assert(!is_hard_linked_ || size == T_BLOCKSIZE);
    assert(size >= header_size_ && blocked_size_ >= size);
    //assert(TH_ISREG(tar_) || size == header_size_);
//    assert(TH_ISDIR(tar_) || TH_ISSYM(tar_) || TH_ISFIFO(tar_) || TH_ISCHR(tar_) || TH_ISBLK(tar_) || th_get_size(tar_) == (size_t)sb.st_size);
}

void TarEntry::rewriteIntoHardLink(TarEntry *target) {
    link_ = target->tarpath_;
    is_hard_linked_ = true;
    updateSizes();
    assert(isHardLink());
}

bool TarEntry::fixHardLink(Path *storage_dir)
{
    debug(HARDLINKS, "Fix hardlink >%s< to >%s< within storage >%s<\n", path_->c_str(), link_->c_str(), storage_dir->c_str());

    if (storage_dir == Path::lookupRoot())
    {
	debug(HARDLINKS, "Nothing to do!\n");
	return true;
    }
    //num_header_blocks -= num_long_link_blocks;
    
    Path *common = Path::commonPrefix(storage_dir, link_);
    debug(HARDLINKS, "COMMON PREFIX >%s< >%s< = >%s<\n", storage_dir->c_str(), link_->c_str(), common?common->c_str():"NULL");
    if (common == NULL || common->depth() < storage_dir->depth()) {
    	warning(HARDLINKS, "Warning: hard link between tars detected! From %s to %s\n", path_->c_str(), link_->c_str());
    	// This hardlink needs to be pushed upwards, into a tar on a higher level!
    	return false;
    }
    else {
	Path *l = link_->subpath(storage_dir->depth());
	debug(HARDLINKS, "CUT LINK >%s< to >%s<\n", link_->c_str(), l->c_str());
	link_ = l;
    }
    //tar_.setHardLink(l->c_str());
    /*
    if (tar_->th_buf.gnu_longlink != NULL) {
        // We needed to use gnu long links, aka an extra header block
        // plus at least one block for the file name. A link target path longer than 512
        // bytes will need a third block etc
        num_long_link_blocks = 2 + l->c_str_len()/T_BLOCKSIZE;
        num_header_blocks += num_long_link_blocks;
        debug(HARDLINKS, "Added %ju blocks for long link header for %s\n",
              num_long_link_blocks, tarpath_->c_str(), l->c_str());
    }
    */
    //tar_.calculateChecksum();
    updateSizes();
    debug(HARDLINKS, "Updated hardlink %s to %s\n", tarpath_->c_str(), link_->c_str());
    return true;
}

void TarEntry::moveEntryToNewParent(TarEntry *entry, TarEntry *parent) {
    auto pos = find(entries_.begin(), entries_.end(), entry);
    if (pos == entries_.end()) {
        error(TARENTRY, "Could not move entry!");
    }
    entries_.erase(pos);
    parent->entries_.insert(parent->entries_.end(), entry);
}

void TarEntry::copyEntryToNewParent(TarEntry *entry, TarEntry *parent) {
    parent->entries_.insert(parent->entries_.end(), entry);
}

/**
 * Update the mtim argument with this entry's mtim, if this entry is younger.
 */
void TarEntry::updateMtim(struct timespec *mtim) {
    if (isInTheFuture(&fs_.st_mtim)) {
        fprintf(stderr, "Entry %s has a future timestamp! Ignoring the timstamp.\n", path()->c_str());
    } else {
        if (fs_.st_mtim.tv_sec > mtim->tv_sec ||
            (fs_.st_mtim.tv_sec == mtim->tv_sec && fs_.st_mtim.tv_nsec > mtim->tv_nsec)) {
            memcpy(mtim, &fs_.st_mtim, sizeof(*mtim));
        }
    }
}

void TarEntry::registerTarFile(TarFile *tf, size_t o) {
    tar_file_ = tf;
    tar_offset_ = o;
}

void TarEntry::registerTazFile() {
    taz_file_ = new TarFile(this, DIR_TAR, 0);
    tars_.push_back(taz_file_);
}

void TarEntry::registerGzFile() {
    gz_file_ = new TarFile(this, REG_FILE, 0);
    tars_.push_back(gz_file_);
}

void TarEntry::registerParent(TarEntry *p) {
    parent_ = p;
}

void TarEntry::secsAndNanos(char *buf, size_t len)
{
    memset(buf, 0, len);
    snprintf(buf, len, "%012ju.%09ju", fs_.st_mtim.tv_sec, fs_.st_mtim.tv_nsec);
}

void TarEntry::injectHash(const char *buf, size_t len)
{
    assert(len<90);
    //memcpy(tar_->th_buf.name+9, buf, len);
    //tar_.calculateChecksum();
}

void TarEntry::addChildrenSize(size_t s)
{
    children_size_ += s;
}

void TarEntry::addDir(TarEntry *dir) {
    dirs_.push_back(dir);
}

void TarEntry::addEntry(TarEntry *te) {
    entries_.push_back(te);
}

void TarEntry::sortEntries() {
    std::sort(entries_.begin(), entries_.end(),
              [](TarEntry *a, TarEntry *b)->bool {
                  return TarSort::lessthan(a->path(), b->path());
              });
}

void TarEntry::calculateHash() {
    calculateSHA256Hash();
}

vector<char> &TarEntry::hash() {
    return sha256_hash_;
}

void TarEntry::calculateSHA256Hash()
{
    SHA256_CTX sha256ctx;
    SHA256_Init(&sha256ctx);

    // Hash the file name and its path within the tar.
    SHA256_Update(&sha256ctx, tarpath_->c_str(), tarpath_->c_str_len());

    // Hash the file size.
    off_t filesize = fs_.st_size;
    fixEndian(&filesize);    
    SHA256_Update(&sha256ctx, &filesize, sizeof(filesize));

    // Hash the last modification time in seconds and nanoseconds.
    time_t secs  = fs_.st_mtim.tv_sec;
    long   nanos = fs_.st_mtim.tv_nsec;
    fixEndian(&secs);
    fixEndian(&nanos);
    SHA256_Update(&sha256ctx, &secs, sizeof(secs));
    SHA256_Update(&sha256ctx, &nanos, sizeof(nanos));

    sha256_hash_.resize(SHA256_DIGEST_LENGTH);
    SHA256_Final((unsigned char*)&sha256_hash_[0], &sha256ctx);
}

void cookEntry(string *listing, TarEntry *entry) {
    
    // -r-------- fredrik/fredrik 745 1970-01-01 01:00 testing
    // drwxrwxr-x fredrik/fredrik   0 2016-11-25 00:52 autoconf/
    // -r-------- fredrik/fredrik   0 2016-11-25 11:23 libtar.so -> libtar.so.0.1
    listing->append(entry->tv_line_left);            
    listing->append(separator_string);
    listing->append(entry->tv_line_size);
    listing->append(separator_string);
    listing->append(entry->tv_line_right);
    listing->append(separator_string);
    listing->append(entry->tarpath()->str());
    listing->append(separator_string);
    if (entry->link() != NULL) {
        if (entry->isSymbolicLink()) {
            listing->append(" -> ");
        } else {
            listing->append(" link to ");
        }
        listing->append(entry->link()->str());
    } else {
        listing->append(" ");
    }
    listing->append(separator_string);
    listing->append(entry->tarFile()->name());
    listing->append(separator_string);
    stringstream ss;
    ss << entry->tarOffset()+entry->headerSize();
    listing->append(ss.str());
    listing->append(separator_string);
    listing->append("0"); // content hash not used
    listing->append(separator_string);
    listing->append(toHex(entry->hash()));
    listing->append("\n");
    listing->append(separator_string);
}

bool eatEntry(vector<char> &v, vector<char>::iterator &i, Path *dir_to_prepend,
              FileStat *fs, size_t *offset, string *tar, Path **path,
              string *link, bool *is_sym_link,
              bool *eof, bool *err)
{
    string permission = eatTo(v, i, separator, 32, eof, err);
    if (*err || *eof) return false;

    fs->st_mode = stringToPermission(permission);
    if (fs->st_mode == 0) {
        *err = true;
        return false;
    }

    string uidgid = eatTo(v, i, separator, 32, eof, err);
    if (*err || *eof) return false;
    string uid = uidgid.substr(0,uidgid.find('/'));
    string gid = uidgid.substr(uidgid.find('/')+1);
    fs->st_uid = atoi(uid.c_str());
    fs->st_gid = atoi(gid.c_str());
    string si = eatTo(v, i, separator, 32, eof, err);
    if (*err || *eof) return false;
    if (fs->isCharacterDevice() || fs->isBlockDevice()) {
        string maj = si.substr(0,si.find(','));
        string min = si.substr(si.find(',')+1);
        fs->st_rdev = MakeDev(atoi(maj.c_str()), atoi(min.c_str()));
    } else {
        fs->st_size = atol(si.c_str());
    }

    string datetime = eatTo(v, i, separator, 32, eof, err);
    if (*err || *eof) return false;

    string secs_and_nanos = eatTo(v, i, separator, 64, eof, err);
    if (*err || *eof) return false;

    // Extract modify time, secs and nanos from string.
    {
        std::vector<char> sn(secs_and_nanos.begin(), secs_and_nanos.end());
        auto j = sn.begin();
        string se = eatTo(sn, j, '.', 64, eof, err);
        if (*err || *eof) return false;
        string na = eatTo(sn, j, -1, 64, eof, err);
        if (*err) return false; // Expect eof here!
        fs->st_mtim.tv_sec = atol(se.c_str());
        fs->st_mtim.tv_nsec = atol(na.c_str());
    }

    secs_and_nanos = eatTo(v, i, separator, 64, eof, err);
    if (*err || *eof) return false;
    
    // Extract access time, secs and nanos from string.
    {
        std::vector<char> sn(secs_and_nanos.begin(), secs_and_nanos.end());
        auto j = sn.begin();
        string se = eatTo(sn, j, '.', 64, eof, err);
        if (*err || *eof) return false;
        string na = eatTo(sn, j, -1, 64, eof, err);
        if (*err) return false; // Expect eof here!
        fs->st_atim.tv_sec = atol(se.c_str());
        fs->st_atim.tv_nsec = atol(na.c_str());
    }

    secs_and_nanos = eatTo(v, i, separator, 64, eof, err);
    if (*err || *eof) return false;    

    // Extract change time, secs and nanos from string.
    {
        std::vector<char> sn(secs_and_nanos.begin(), secs_and_nanos.end());
        auto j = sn.begin();
        string se = eatTo(sn, j, '.', 64, eof, err);
        if (*err || *eof) return false;
        string na = eatTo(sn, j, -1, 64, eof, err);
        if (*err) return false; // Expect eof here!
        fs->st_ctim.tv_sec = atol(se.c_str());
        fs->st_ctim.tv_nsec = atol(na.c_str());
    }
    
    string filename = dir_to_prepend->str() + "/" + eatTo(v, i, separator, 1024, eof, err);
    if (*err || *eof) return false;
    if (filename.length() > 1 && filename.back() == '/')
    {
        filename = filename.substr(0, filename.length() - 1);
    }
    *path = Path::lookup(filename);
    *link = eatTo(v, i, separator, 1024, eof, err);
    if (*err || *eof) return false;
    *is_sym_link = false;
    if (link->length() > 4 && link->substr(0, 4) == " -> ")
    {
        *link = link->substr(4);
        fs->st_size = link->length();
        *is_sym_link = true;
    }
    else if (link->length() > 9 && link->substr(0, 9) == " link to ")
    {
        *link = link->substr(9);
        fs->st_size = link->length();
        *is_sym_link = false;
    }
    *tar = dir_to_prepend->str() + "/" + eatTo(v, i, separator, 1024, eof, err);
    if (*err || *eof) return false;
    string off = eatTo(v, i, separator, 32, eof, err);
    *offset = atol(off.c_str());
    if (*err || *eof) return false;
    string content_hash = eatTo(v, i, separator, 65, eof, err);
    if (*err || *eof) return false;
    string header_hash = eatTo(v, i, separator, 65, eof, err);    
    header_hash.pop_back(); // Remove the newline
    if (*err) return false; // Accept eof here!
    return true;
}
                
