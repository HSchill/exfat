/*
	repair.c (09.03.17)
	exFAT file system implementation library.

	Free exFAT implementation.
	Copyright (C) 2010-2017  Andrew Nayenko

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License along
	with this program; if not, write to the Free Software Foundation, Inc.,
	51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "exfat.h"
#include <strings.h>

int exfat_errors_fixed;

bool exfat_ask_to_fix(const struct exfat* ef)
{
	const char* question = "Fix (Y/N)?";
	char answer[8];
	bool yeah, nope;

	switch (ef->repair)
	{
	case EXFAT_REPAIR_NO:
		return false;
	case EXFAT_REPAIR_YES:
		printf("%s %s", question, "Y\n");
		return true;
	case EXFAT_REPAIR_ASK:
		do
		{
			printf("%s ", question);
			fflush(stdout);
			fgets(answer, sizeof(answer), stdin);

			yeah = strcasecmp(answer, "Y\n") == 0;
			nope = strcasecmp(answer, "N\n") == 0;
		}
		while (!yeah && !nope);
		return yeah;
	}
	exfat_bug("invalid repair option value: %d", ef->repair);
}
