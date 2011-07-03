// yfs client.  implements FS operations using extent and lock server
#include "yfs_client.h"
#include "extent_client.h"
#include <sstream>
#include <iostream>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include "zdebug.h"

yfs_client::yfs_client(std::string extent_dst, std::string lock_dst)
{
  ec = new extent_client(extent_dst);
    srand(time(NULL));
}

yfs_client::inum yfs_client::rand_inum(bool isfile) {
    inum ret = 0;
    ret = (unsigned long long) ( (rand() & 0x7fffffff) | (isfile << 31) );
    ret &= 0xffffffff;
    return ret;
}

yfs_client::inum
yfs_client::n2i(std::string n)
{
  std::istringstream ist(n);
  unsigned long long finum;
  ist >> finum;
  return finum;
}

std::string
yfs_client::filename(inum inum)
{
  std::ostringstream ost;
  ost << inum;
  return ost.str();
}

bool
yfs_client::isfile(inum inum)
{
  if(inum & 0x80000000)
    return true;
  return false;
}

bool
yfs_client::isdir(inum inum)
{
  return ! isfile(inum);
}

int
yfs_client::getfile(inum inum, fileinfo &fin)
{
  int r = OK;

  printf("getfile %016llx\n", inum);
  extent_protocol::attr a;
  if (ec->getattr(inum, a) != extent_protocol::OK) {
    r = IOERR;
    goto release;
  }

  fin.atime = a.atime;
  fin.mtime = a.mtime;
  fin.ctime = a.ctime;
  fin.size = a.size;
  printf("getfile %016llx -> sz %llu\n", inum, fin.size);

 release:

  return r;
}

int
yfs_client::getdir(inum inum, dirinfo &din)
{
  int r = OK;

  printf("getdir %016llx\n", inum);
  extent_protocol::attr a;
  if (ec->getattr(inum, a) != extent_protocol::OK) {
    r = IOERR;
    goto release;
  }
  din.atime = a.atime;
  din.mtime = a.mtime;
  din.ctime = a.ctime;

 release:
  return r;
}

int yfs_client::put(inum num, std::string buf) {
    Z("put: %llx %s\n", num, buf.c_str());
    extent_protocol::status rs;
    rs = ec->put(num, buf);
    if (rs == extent_protocol::OK) {
        return OK;
    }
    return IOERR;
}

int yfs_client::get(inum num, std::string &buf) {
    extent_protocol::status rs = ec->get(num, buf);
    if (rs == extent_protocol::OK) {
        return OK;
    }
    return IOERR;
}

int yfs_client::create(inum parent, const char * name, unsigned long &ino) {
    Z("create : parentis %lld name is %s\n", parent, name);
    if (isdir(parent)) {
        std::string b;
        int rs = get(parent, b);
        if (rs != OK) {
            return rs;
        }
        std::string t = "/" + std::string(name) + "/";
        if (b.find(t) != std::string::npos) {
            Z("create file exist !!!!!\n");
            return EXIST;
        }
        inum num = rand_inum();
        ino = (unsigned long)(num & 0xffffffff);
        b = b.append(filename(num) + t);
        rs = put(num, "");
        if (rs != OK) return rs;
        rs = put(parent, b);
        if (rs != OK) return rs;
        return OK;
    }
    return NOENT;
}

bool yfs_client::lookup(inum parent, const char *name, unsigned long &ino) {
    Z("parent %lld name '%s'\n", parent, name);
    if (isdir(parent)) {
        //printf("%d %d \n", name == NULL, strlen(name));
        if (name == NULL || strlen(name) < 1) return true;
        std::string b;
        int rs = get(parent, b);
        if (rs != OK) {
            return false;
        }
        std::string t = "/" + std::string(name) + "/";
        size_t found = b.find(t);
        if (found != std::string::npos) {
            assert(found > 0);
            size_t left = b.rfind('/', found - 1);
            if (left == std::string::npos) {
                left = 0;
            } else {
                left++;
            }
            assert(found > left);
            ino = n2i(b.substr(left, found - left));
            
            return true;
        }
    }
    return false;
}

int yfs_client::read(inum ino, size_t size, off_t off, std::string &ret) {
    std::string buf;
    int rs = get(ino, buf);
    if (rs != OK) {
        return rs;
    }
    if (off + size > buf.size()) {
        int reside = off + size - buf.size();
        char * a = new char[reside];
        bzero(a, off + size - buf.size());
        ret = buf.substr(off, buf.size() - off).append(std::string(a, reside));
    } else {
        ret = buf.substr(off, size);
    }
    return OK;
}

int yfs_client::write(inum ino, const char * buf, size_t size, off_t off) {
    if (off < 0) {
        return NOENT;
    }
    size_t uoff = (unsigned)off;
    std::string ori;
    int rs = get(ino, ori);
    if (rs != OK) {
        return rs;
    }
    std::string after;
    if (uoff <= ori.size() || !uoff) {
        after = ori.substr(0, uoff).append(std::string(buf, size));
        if (uoff + size < ori.size()) {
            after.append(ori.substr(uoff + size, ori.size() - uoff - size));
        }
    } else {
        size_t gap = uoff - ori.size();
        char * a = new char[gap];
        bzero(a, gap);
        after = ori.append(std::string(a, gap));
        after = after.append(std::string(buf, size));
    }
    rs = put(ino, after);
    return rs;
}

int yfs_client::setattr(inum fileno, struct stat *attr) {
    std::string buf;
    int rs = get(fileno, buf);
    if (rs != OK) {
        return rs;
    }
    unsigned int sz = buf.size();
    if (sz < attr->st_size) {
        char * a = new char[attr->st_size - sz];
        buf.append(std::string(a));
    } else {
        buf = buf.substr(0, attr->st_size);
    }
    rs = put(fileno, buf);
    return rs;
}
