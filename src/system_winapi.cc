/*
 Copyright (C) 2017 Fredrik Öhrström

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

#include "log.h"
#include "system.h"

#include <sys/types.h>

using namespace std;

//static ComponentId SYSTEM = registerLogComponent("system");

struct SystemImplementationWinapi : System
{
    int invoke(string program,
               vector<string> args,
               vector<char> *out);
};

unique_ptr<System> newSystem()
{
    return unique_ptr<System>(new SystemImplementationWinapi());
}

string protect_(string arg)
{
    return arg;
}

int SystemImplementationWinapi::invoke(string program,
                                       vector<string> args,
                                       vector<char> *out)
{
    return OK;
}