mod_reproxy for lighttpd 1.4.x
==============================

This module add `X-Reproxy-URL` header support to lighttpd 1.4.x.


Installation
------------

1. Download mod_reproxy.c
2. Copy it into lighttpd src directory
3. Edit src/Makefile.am and add this after last module

    lib_LTLIBRARIES += mod_reproxy.la
    mod_reproxy_la_SOURCES = mod_reproxy.c
    mod_reproxy_la_LDFLAGS = -module -export-dynamic -avoid-version -no-undefined
    mod_reproxy_la_LIBADD = $(common_libadd)

4. and build lighttpd by following commands

    ./autogen.sh
    ./configure ...
    make && make install


Usage
-----

load mod_reproxy

    server.modules = (
        "mod_reproxy", ...
    )

And insert following line to the scope where you want to use X-Reproxy-URL:

    reproxy.enable = "enable"

In this scope, you can use `X-Reproxy-URL` header in any other module that use subrequest. (ex: mod_cgi, mod_fastcgi, mod_proxy, and etc)


FAQ
---

* There's no 1.5 support?
** No. Because lighttpd 1.5 has already same feature known as `X-Rewrite-*`


LICENSE
-------

BSD. Same as lighttpd
