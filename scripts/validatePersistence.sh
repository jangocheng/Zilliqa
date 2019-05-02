#!/bin/bash
# Copyright (C) 2019 Zilliqa
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.


zilliqa_path="/usr/local"
folder_name="validateDB"


if [ ! -f "./constants.xml" ] || [ ! -f "./dsnodes.xml"] || [ ! -f "./persistence"]; then
	echo "The workind directory must have constants.xml, dsnodes.xml and persistence folder"
	return 1;
fi

mkdir $folder_name
cd $folder_name
cp ../constants.xml .
cp ../dsnodes.xml .
cp -r ../persistence .

echo -n "Enter the full path of your zilliqa source code directory: default = ""${zilliqa_path}" && read path_read && [ -n "$path_read" ] && zilliqa_path=$path_read

if [ -z "$zilliqa_path" ] || ([ ! -x $zilliqa_path/build/bin/validateDB ] && [ ! -x $zilliqa_path/bin/validateDB ]); then
    echo "Cannot find zilliqa binary on the path you specified"
    exit 1
fi

if [ -x $zilliqa_path/build/bin/validateDB ]; then
    $zilliqa_path/build/bin/validateDB
elif [ -x $zilliqa_path/bin/validateDB ]; then
    $zilliqa_path/bin/validateDB
fi
