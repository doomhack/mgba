// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#include "elf++.hh"

#include <system_error>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

using namespace std;

ELFPP_BEGIN_NAMESPACE

class mmap_loader : public loader
{
        void *base;
        size_t lim;

public:
        mmap_loader(FILE* fd)
        {
                fseek(fd, 0, SEEK_END);
		        int end = ftell(fd);

                if (end == (off_t)-1)
                        throw system_error(errno, system_category(),
                                           "finding file length");
                lim = end;

                //base = mmap(nullptr, lim, PROT_READ, MAP_SHARED, fd, 0);
                base = new char[lim];
                fseek(fd, 0, SEEK_SET);

                fread(base, lim, 1, fd);

                //if (base == MAP_FAILED)
                //        throw system_error(errno, system_category(),"mmap'ing file");
                fclose(fd);
        }

        ~mmap_loader()
        {
            delete[] (char*)base;
                //munmap(base, lim);
        }

        const void *load(off_t offset, size_t size)
        {
                if (offset + size > lim)
                        throw range_error("offset exceeds file size");
                return (const char*)base + offset;
        }
};

std::shared_ptr<loader>
create_mmap_loader(FILE* fd)
{
        return make_shared<mmap_loader>(fd);
}

ELFPP_END_NAMESPACE
