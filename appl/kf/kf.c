/*
 * Copyright (c) 1997 - 1999 Kungliga Tekniska H�gskolan
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
 * 3. All advertising materials mentioning features or use of this software 
 *    must display the following acknowledgement: 
 *      This product includes software developed by Kungliga Tekniska 
 *      H�gskolan and its contributors. 
 *
 * 4. Neither the name of the Institute nor the names of its contributors 
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

#include "kf_locl.h"
RCSID("$Id$");

krb5_context context;
static int help_flag;
static int version_flag;
static char *port_str;
char *service = SERVICE;
char *remote_name=NULL;
int forwardable=0;
char *tk_file=NULL;

static struct getargs args[] = {
    { "port", 'p', arg_string, &port_str, "port to connect to", "port" },
    { "login", 'l',arg_string, &remote_name,"remote login name","login"},
    { "tkfile", 't',arg_string, &tk_file,"remote ticket file","tkfile"},
    { "forwardable",'F',arg_flag,&forwardable,
       "Forward forwardable credentials", NULL },
    { "help", 'h', arg_flag, &help_flag },
    { "version", 0, arg_flag, &version_flag }
};

static int num_args = sizeof(args) / sizeof(args[0]);

static void
usage(int code, struct getargs *args, int num_args)
{
    arg_printusage(args, num_args, NULL, "hosts");
    exit(code);
}

static int
client_setup(krb5_context *context, int *argc, char **argv)
{
    int optind;
    int port = 0;

    optind = krb5_program_setup(context, *argc, argv, args, num_args, usage);

    if(help_flag)
	(*usage)(0, args, num_args);
    if(version_flag) {
	print_version(NULL);
	exit(0);
    }
    
    if(port_str){
	struct servent *s = roken_getservbyname(port_str, "tcp");
	if(s)
	    port = s->s_port;
	else {
	    char *ptr;

	    port = strtol (port_str, &ptr, 10);
	    if (port == 0 && ptr == port_str)
		errx (1, "Bad port `%s'", port_str);
	    port = htons(port);
	}
    }

    if (port == 0)
	port = krb5_getportbyname (*context, PORT, "tcp", PORT_NUM);
   
    if(*argc - optind < 1)
        usage(1, args, num_args);
    *argc=optind;

    return port;
}

static int
proto (int sock, const char *hostname, const char *service)
{
    struct sockaddr_in remote, local;
    int addrlen;
    krb5_address remote_addr, local_addr;
    krb5_context context;
    krb5_auth_context auth_context;
    krb5_error_code status;
    krb5_principal server;
    krb5_data data;
    krb5_data packet;
    krb5_data data_send;
    u_int32_t len, net_len;

    krb5_ccache     ccache;
    krb5_creds      creds;
    krb5_kdc_flags  flags;
    krb5_principal  principal;
    struct passwd *pwd;
    char ret_string[10];
    char buf[1000];

    addrlen = sizeof(local);
    if (getsockname (sock, (struct sockaddr *)&local, &addrlen) < 0
	|| addrlen != sizeof(local))
	{warn ("getsockname(%s)", hostname);
         return 1;
    }

    addrlen = sizeof(remote);
    if (getpeername (sock, (struct sockaddr *)&remote, &addrlen) < 0
	|| addrlen != sizeof(remote))
	{warn ("getpeername(%s)", hostname);
         return 1;
    }

    status = krb5_init_context(&context);
    if (status)
	errx (1, "krb5_init_context: %s",
	      krb5_get_err_text(context, status));

    status = krb5_auth_con_init (context, &auth_context);
    if (status)
	{warn ("krb5_auth_con_init: %s",
	      krb5_get_err_text(context, status));
         return 1;
    }

    local_addr.addr_type = AF_INET;
    local_addr.address.length = sizeof(local.sin_addr);
    local_addr.address.data   = &local.sin_addr;

    remote_addr.addr_type = AF_INET;
    remote_addr.address.length = sizeof(remote.sin_addr);
    remote_addr.address.data   = &remote.sin_addr;

    status = krb5_auth_con_setaddrs (context,
				     auth_context,
				     &local_addr,
				     &remote_addr);
    if (status)
	{warn ("krb5_auth_con_setaddr: %s",
	      krb5_get_err_text(context, status));
         return 1;
    }

    status = krb5_sname_to_principal (context,
				      hostname,
				      service,
				      KRB5_NT_SRV_HST,
				      &server);
    if (status)
	{warn ("krb5_sname_to_principal: %s",
	      krb5_get_err_text(context, status));
         return 1;
        }

    status = krb5_sendauth (context,
			    &auth_context,
			    &sock,
			    VERSION,
			    NULL,
			    server,
			    AP_OPTS_MUTUAL_REQUIRED,
			    NULL,
			    NULL,
			    NULL,
			    NULL,
			    NULL,
			    NULL);
    if (status)
	{warn("krb5_sendauth: %s",
	      krb5_get_err_text(context, status));
         return 1;
        }

    if (remote_name == NULL)
      {
        pwd = getpwuid (getuid());
        if (pwd == NULL)
           errx (1, "who are you?");
        remote_name=pwd->pw_name;
      }

    krb5_data_zero(&data_send);
    data_send.data=remote_name;
    data_send.length=strlen(remote_name) + 1;
    status=krb5_write_message(context,&sock,&data_send);
    if (status)
       {err(1,"krb5_write_message");
    }
  
    if (tk_file) snprintf (buf, sizeof(buf), "%s", tk_file);  
      else snprintf (buf, sizeof(buf), "");
    data_send.data=buf;
    data_send.length=strlen(buf)+1;
    status=krb5_write_message(context,&sock,&data_send);
    if (status)
       {err(1,"krb5_write_message");
    }

    memset (&creds, 0, sizeof(creds));

    status = krb5_cc_default (context, &ccache);
    if (status) {
	warn ("could not forward creds: krb5_cc_default: %s",
	       krb5_get_err_text (context, status));
    }

    status = krb5_cc_get_principal (context, ccache, &principal);
    if (status) {
	warn ("could not forward creds: krb5_cc_get_principal: %s",
	       krb5_get_err_text (context, status));
	return 1;
    }

    creds.client = principal;
    
    status = krb5_build_principal (context,
				&creds.server,
				strlen(principal->realm),
				principal->realm,
				"krbtgt",
				principal->realm,
				NULL);

    if (status) {
	warn ("could not forward creds: krb5_build_principal: %s",
	       krb5_get_err_text (context, status));
	return 1;
    }

    creds.times.endtime = 0;

    flags.i = 0;
    flags.b.forwarded   = 1;
    flags.b.forwardable = forwardable;

    status = krb5_get_forwarded_creds (context,
				    auth_context,
				    ccache,
				    flags.i,
				    hostname,
				    &creds,
				    &data);
    if (status) {
	warn ("could not forward creds: krb5_get_forwarded_creds: %s",
	       krb5_get_err_text (context, status));
	return 1;
    }

    status = krb5_mk_priv (context,
                           auth_context,
                           &data,
                           &packet,
                           NULL);
    if (status)
        {warn ("krb5_mk_priv: %s",
              krb5_get_err_text(context, status));
         return 1;}
    
    len = packet.length;
    net_len = htonl(len);

    if (krb5_net_write (context, &sock, &net_len, 4) != 4)
        {warn("krb5_net_write");
         return 1;}
    if (krb5_net_write (context, &sock, packet.data, len) != len)
        {warn ("krb5_net_write");
         return 1;}

    krb5_data_free (&data);

    if (krb5_net_read (context, &sock, &net_len, 4) != 4)
        err (1, "krb5_net_read");
    len = ntohl(net_len);
    if (krb5_net_read (context, &sock, ret_string, len) != len)
        err (1, "krb5_net_read");

    return(strcmp(ret_string,"ok"));
}

static int
doit (const char *hostname, int port, const char *service)
{
    struct in_addr **h;
    struct hostent *hostent;

    hostent = roken_gethostbyname (hostname);
    if (hostent == NULL)
	{warn ("gethostbyname '%s' failed: %s",
	      hostname,
	      hstrerror(h_errno));
         return 1;
    }

    for (h = (struct in_addr **)hostent->h_addr_list;
	*h != NULL;
	 ++h) {
	struct sockaddr_in addr;
	int s;

	memset (&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = port;
	addr.sin_addr   = **h;

	s = socket (AF_INET, SOCK_STREAM, 0);
	if (s < 0)
	    err (1, "socket");
	if (connect (s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
	    warn ("connect(%s)", hostname);
	    close (s);
	    continue;
	}
	return proto (s, hostname, service);
    }
    return 1;
}

int
main(int argc, char **argv)
{ int argcc,port,i;
  int ret=0;
 
  argcc=argc;
  port=client_setup(&context, &argcc, argv);

  for (i=argcc;i<argc;i++)
     {ret=doit (argv[i], port, service);
      printf("%s:%s\n",argv[i],ret ? "failed" : "ok");
     }
  return(ret);
}
