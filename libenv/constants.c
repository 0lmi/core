/*
  Copyright 2023 Northern.tech AS

  This file is part of CFEngine 3 - written and maintained by Northern.tech AS.

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; version 3.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include <constants.h>

#include <eval_context.h>
#include <file_lib.h>

void LoadSystemConstants(EvalContext *ctx)
{
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_CONST, "at", "@", CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_CONST, "dollar", "$", CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_CONST, "n", "\n", CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_CONST, "r", "\r", CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_CONST, "t", "\t", CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_CONST, "endl", "\n", CF_DATA_TYPE_STRING, "source=agent");
    EvalContextVariablePutSpecial(ctx, SPECIAL_SCOPE_CONST, "dirsep", FILE_SEPARATOR_STR, CF_DATA_TYPE_STRING, "source=agent");
    /* NewScalar("const","0","\0",cf_str);  - this cannot work */
}
