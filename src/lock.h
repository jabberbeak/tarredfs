/*
    Copyright (C) 2016-2018 Fredrik Öhrström

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

#ifndef LOCK_H
#define LOCK_H

#include <pthread.h>

void lockMutex(pthread_mutex_t *lock, const char *func, const char *file, int line);
void unlockMutex(pthread_mutex_t *lock, const char *func, const char *file, int line);

#define LOCK(l) lockMutex(l, __func__, __FILE__, __LINE__)
#define UNLOCK(l) unlockMutex(l, __func__, __FILE__, __LINE__)

#endif
