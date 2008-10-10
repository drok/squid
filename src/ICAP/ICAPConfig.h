
/*
 * $Id: ICAPConfig.h,v 1.17 2008/02/12 23:12:45 rousskov Exp $
 *
 *
 * SQUID Web Proxy Cache          http://www.squid-cache.org/
 * ----------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from
 *  the Internet community; see the CONTRIBUTORS file for full
 *  details.   Many organizations have provided support for Squid's
 *  development; see the SPONSORS file for full details.  Squid is
 *  Copyrighted (C) 2001 by the Regents of the University of
 *  California; see the COPYRIGHT file for full details.  Squid
 *  incorporates software developed and/or copyrighted by other
 *  sources; see the CREDITS file for full details.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111, USA.
 *
 *
 * Copyright (c) 2003, Robert Collins <robertc@squid-cache.org>
 */

#ifndef SQUID_ICAPCONFIG_H
#define SQUID_ICAPCONFIG_H

#include "event.h"
#include "AsyncCall.h"
#include "adaptation/Config.h"
#include "ICAPServiceRep.h"

class acl_access;

class ConfigParser;

class ICAPConfig: public Adaptation::Config
{

public:
    int default_options_ttl;
    int preview_enable;
    int preview_size;
    time_t connect_timeout_raw;
    time_t io_timeout_raw;
    int reuse_connections;
    char* client_username_header;
    int client_username_encode;

    ICAPConfig();
    ~ICAPConfig();

    time_t connect_timeout(bool bypassable) const;
    time_t io_timeout(bool bypassable) const;

private:
    ICAPConfig(const ICAPConfig &); // not implemented
    ICAPConfig &operator =(const ICAPConfig &); // not implemented

    virtual Adaptation::ServicePointer createService(const Adaptation::ServiceConfig &cfg);
};

extern ICAPConfig TheICAPConfig;

#endif /* SQUID_ICAPCONFIG_H */
