/*
 * Copyright (c) 2000 Kungliga Tekniska H�gskolan
 * (Royal Institute of Technology, Stockholm, Sweden). 
 * All rights reserved. 
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 *
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution. 
 *
 * 3. Neither the name of the Institute nor the names of its contributors 
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE 
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS 
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE. 
 */

#include "kadmin_locl.h"
#include <sys/select.h>

RCSID("$Id$");

struct kadm_port {
    char *port;
    unsigned short def_port;
    struct kadm_port *next;
} *kadm_ports;

static void
add_kadm_port(krb5_context context, const char *service, unsigned int port)
{
    struct kadm_port *p;
    p = malloc(sizeof(*p));
    if(p == NULL) {
	krb5_warnx(context, "failed to allocate %lu bytes\n", 
		   (unsigned long)sizeof(*p));
	return;
    }
    
    p->port = strdup(service);
    p->def_port = port;

    p->next = kadm_ports;
    kadm_ports = p;
}

static void
add_standard_ports (krb5_context context)
{
    add_kadm_port(context, "kerberos-adm", 749);
#ifdef KRB4
    add_kadm_port(context, "kerberos-master", 751);
#endif
}

/*
 * parse the set of space-delimited ports in `str' and add them.
 * "+" => all the standard ones
 * otherwise it's port|service[/protocol]
 */

void
parse_ports(krb5_context context, const char *str)
{
    char p[128];

    while(strsep_copy(&str, " \t", p, sizeof(p)) != -1) {
	if(strcmp(p, "+") == 0)
	    add_standard_ports(context);
	else
	    add_kadm_port(context, p, 0);
    }
}

static pid_t pgrp;
int term_flag;

void
wait_term(int sig)
{
    term_flag = 1;
}

void
terminate(int sig)
{
    killpg(pgrp, sig);
}

static int
wait_for_connection(krb5_context context,
		    int *socks, int num_socks)
{
    int i, e;
    fd_set orig_read_set, read_set;
    int max_fd = -1;
    
    FD_ZERO(&orig_read_set);
    
    for(i = 0; i < num_socks; i++) {
	FD_SET(socks[i], &orig_read_set);
	max_fd = max(max_fd, socks[i]);
    }
    
    signal(SIGTERM, terminate);

    if(setpgid(0, getpid()) < 0)
	err(1, "setpgid");

    pgrp = getpid();

    while (1) {
	read_set = orig_read_set;
	e = select(max_fd + 1, &read_set, NULL, NULL, NULL);
	if(e < 0)
	    krb5_warn(context, errno, "select");
	else if(e == 0)
	    krb5_warnx(context, "select returned 0");
	else {
	    for(i = 0; i < num_socks; i++) {
		if(FD_ISSET(socks[i], &read_set)) {
		    struct sockaddr sa;
		    size_t sa_len;
		    int s;
		    pid_t pid;
		    krb5_address addr;
		    char buf[128];
		    size_t buf_len;
		    s = accept(socks[i], &sa, &sa_len);
		    if(s < 0) {
			krb5_warn(context, errno, "accept");
			continue;
		    }
		    e = krb5_sockaddr2address(&sa, &addr);
		    if(e)
			krb5_warn(context, e, "krb5_sockaddr2address");
		    else {
			e = krb5_print_address (&addr, buf, sizeof(buf), 
						&buf_len);
			if(e) 
			    krb5_warn(context, e, "krb5_sockaddr2address");
			else
			    krb5_warnx(context, "connection from %s", buf);
			krb5_free_address(context, &addr);
		    }
		    
		    pid = fork();
		    if(pid == 0) {
			signal(SIGTERM, wait_term);
			for(i = 0; i < num_socks; i++)
			    close(socks[i]);
			dup2(s, STDIN_FILENO);
			dup2(s, STDOUT_FILENO);
			if(s != STDIN_FILENO && s != STDOUT_FILENO)
			    close(s);
			return 0;
		    }
		}
	    }
	}
    }
}


int
start_server(krb5_context context)
{
    int e;
    struct kadm_port *p;

    int *socks = NULL, *tmp;
    int num_socks = 0;
    int i;

    for(p = kadm_ports; p; p = p->next) {
	struct addrinfo hints, *ai, *ap;
	char portstr[32];
	memset (&hints, 0, sizeof(hints));
	hints.ai_flags    = AI_PASSIVE;
	hints.ai_socktype = SOCK_STREAM;

	e = getaddrinfo(NULL, p->port, &hints, &ai);
	if(e) {
	    snprintf(portstr, sizeof(portstr), "%u", p->def_port);
	    e = getaddrinfo(NULL, portstr, &hints, &ai);
	}

	if(e) {
	    krb5_warn(context, krb5_eai_to_heim_errno(e), "%s", portstr);
	    continue;
	}
	i = 0;
	for(ap = ai; ap; ap = ap->ai_next) 
	    i++;
	tmp = realloc(socks, (num_socks + i) * sizeof(*socks));
	if(tmp == NULL) {
	    krb5_warnx(context, "failed to reallocate %lu bytes", 
		       (unsigned long)(num_socks + i) * sizeof(*socks));
	    continue;
	}
	socks = tmp;
	for(ap = ai; ap; ap = ap->ai_next) {
	    int one = 1;
	    int s = socket(ap->ai_family, ap->ai_socktype, ap->ai_protocol);
	    if(s < 0) {
		krb5_warn(context, errno, "socket");
		continue;
	    }
#if defined(SO_REUSEADDR) && defined(HAVE_SETSOCKOPT)
	    if(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void *)&one,
			  sizeof(one)) < 0)
		krb5_warn(context, errno, "setsockopt");
#endif
	    if (bind (s, ap->ai_addr, ap->ai_addrlen) < 0) {
		krb5_warn(context, errno, "bind");
		close(s);
		continue;
	    }
	    if (listen (s, SOMAXCONN) < 0) {
		krb5_warn(context, errno, "listen");
		close(s);
		continue;
	    }
	    socks[num_socks++] = s;
	}
	freeaddrinfo (ai);
    }
    if(num_socks == 0)
	krb5_errx(context, 1, "no sockets to listen to - exiting");
    return wait_for_connection(context, socks, num_socks);
}
