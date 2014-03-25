/*
 * Daemon that gets requests from remote library backend
 * and forwards them to local session.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include "uapi/libct.h"
#include "list.h"
#include "xmalloc.h"
#include "fs.h"
#include "../protobuf/rpc.pb-c.h"

#define MAX_MSG_ONSTACK	2048
#define BADCTRID_ERR	-42

struct container_srv {
	struct list_head l;
	unsigned long rid;
	ct_handler_t hnd;
};

static LIST_HEAD(ct_srvs);
static unsigned long rids = 1;

static struct container_srv *find_ct_by_rid(unsigned long rid)
{
	struct container_srv *cs;

	list_for_each_entry(cs, &ct_srvs, l)
		if (cs->rid == rid)
			return cs;

	return NULL;
}

static int send_err_resp(int sk, int err)
{
	RpcResponce resp = RPC_RESPONCE__INIT;
	unsigned char dbuf[MAX_MSG_ONSTACK];
	int len;

	resp.success = false;
	len = rpc_responce__pack(&resp, dbuf);
	if (len > 0)
		send(sk, dbuf, len, 0);

	return 0;
}

static int send_resp(int sk, int err, RpcResponce *resp)
{
	unsigned char dbuf[MAX_MSG_ONSTACK];
	int len;

	if (err)
		return send_err_resp(sk, err);

	resp->success = true;

	/* FIXME -- boundaries check */
	len = rpc_responce__pack(resp, dbuf);
	if (send(sk, dbuf, len, 0) != len)
		return -1;
	else
		return 0;
}

static int serve_ct_create(int sk, libct_session_t ses, CreateReq *req)
{
	struct container_srv *cs;
	RpcResponce resp = RPC_RESPONCE__INIT;
	CreateResp cr = CREATE_RESP__INIT;

	cs = xmalloc(sizeof(*cs));
	if (!cs)
		goto err0;

	cs->hnd = libct_container_create(ses);
	if (!cs->hnd)
		goto err1;

	cs->rid = rids++;
	resp.create = &cr;
	cr.rid = cs->rid;
	if (send_resp(sk, 0, &resp))
		goto err2;

	list_add_tail(&cs->l, &ct_srvs);
	return 0;

err2:
	libct_container_destroy(cs->hnd);
err1:
	xfree(cs);
err0:
	return send_err_resp(sk, -1);
}

static int serve_ct_destroy(int sk, struct container_srv *cs, RpcRequest *req)
{
	RpcResponce resp = RPC_RESPONCE__INIT;

	list_del(&cs->l);
	libct_container_destroy(cs->hnd);
	xfree(cs);

	return send_resp(sk, 0, &resp);
}

static int serve_get_state(int sk, struct container_srv *cs, RpcRequest *req)
{
	RpcResponce resp = RPC_RESPONCE__INIT;
	StateResp gs = STATE_RESP__INIT;

	resp.state = &gs;
	gs.state = libct_container_state(cs->hnd);

	return send_resp(sk, 0, &resp);
}

static int serve_spawn(int sk, struct container_srv *cs, RpcRequest *req)
{
	RpcResponce resp = RPC_RESPONCE__INIT;
	int ret = -1;

	if (req->spawn)
		ret = libct_container_spawn_execv(cs->hnd, req->spawn->path, req->spawn->args);

	return send_resp(sk, ret, &resp);
}

static int serve_kill(int sk, struct container_srv *cs, RpcRequest *req)
{
	RpcResponce resp = RPC_RESPONCE__INIT;
	int ret;

	ret = libct_container_kill(cs->hnd);
	return send_resp(sk, ret, &resp);
}

static int serve_wait(int sk, struct container_srv *cs, RpcRequest *req)
{
	RpcResponce resp = RPC_RESPONCE__INIT;
	int ret;

	ret = libct_container_wait(cs->hnd);
	return send_resp(sk, ret, &resp);
}

static int serve_setnsmask(int sk, struct container_srv *cs, RpcRequest *req)
{
	RpcResponce resp = RPC_RESPONCE__INIT;
	int ret = -1;

	if (req->nsmask)
		ret = libct_container_set_nsmask(cs->hnd, req->nsmask->mask);
	return send_resp(sk, ret, &resp);
}

static int serve_addcntl(int sk, struct container_srv *cs, RpcRequest *req)
{
	RpcResponce resp = RPC_RESPONCE__INIT;
	int ret = -1;

	if (req->addcntl)
		ret = libct_container_add_controller(cs->hnd, req->addcntl->ctype);
	return send_resp(sk, ret, &resp);
}

static int serve_setroot(int sk, struct container_srv *cs, RpcRequest *req)
{
	RpcResponce resp = RPC_RESPONCE__INIT;
	int ret = -1;

	if (req->setroot)
		ret = libct_fs_set_root(cs->hnd, req->setroot->root);
	return send_resp(sk, ret, &resp);
}

static int serve_setpriv(int sk, struct container_srv *cs, RpcRequest *req)
{
	RpcResponce resp = RPC_RESPONCE__INIT;
	int ret = -1;

	if (req->setpriv) {
		const struct ct_fs_ops *ops;

		ops = fstype_get_ops(req->setpriv->type);
		if (ops) {
			void *arg;

			arg = ops->pb_unpack(req->setpriv);
			ret = libct_fs_set_private(cs->hnd, req->setpriv->type, arg);
			xfree(arg);
		}
	}

	return send_resp(sk, ret, &resp);
}

static int serve_set_option(int sk, struct container_srv *cs, RpcRequest *req)
{
	RpcResponce resp = RPC_RESPONCE__INIT;
	int ret = -1, opt = -1;

	if (req->setopt)
		opt = req->setopt->opt;

	switch (opt) {
	case LIBCT_OPT_AUTO_PROC_MOUNT:
		ret = libct_container_set_option(cs->hnd, opt);
		break;
	}

	return send_resp(sk, ret, &resp);
}

static int serve_req(int sk, libct_session_t ses, RpcRequest *req)
{
	struct container_srv *cs = NULL;

	if (req->req == REQ_TYPE__CT_CREATE)
		return serve_ct_create(sk, ses, req->create);

	if (req->has_ct_rid)
		cs = find_ct_by_rid(req->ct_rid);
	if (!cs)
		return send_err_resp(sk, BADCTRID_ERR);

	switch (req->req) {
	case REQ_TYPE__CT_DESTROY:
		return serve_ct_destroy(sk, cs, req);
	case REQ_TYPE__CT_GET_STATE:
		return serve_get_state(sk, cs, req);
	case REQ_TYPE__CT_SPAWN:
		return serve_spawn(sk, cs, req);
	case REQ_TYPE__CT_KILL:
		return serve_kill(sk, cs, req);
	case REQ_TYPE__CT_WAIT:
		return serve_wait(sk, cs, req);
	case REQ_TYPE__CT_SETNSMASK:
		return serve_setnsmask(sk, cs, req);
	case REQ_TYPE__CT_ADD_CNTL:
		return serve_addcntl(sk, cs, req);
	case REQ_TYPE__FS_SETROOT:
		return serve_setroot(sk, cs, req);
	case REQ_TYPE__FS_SETPRIVATE:
		return serve_setpriv(sk, cs, req);
	case REQ_TYPE__CT_SET_OPTION:
		return serve_set_option(sk, cs, req);
	default:
		break;
	}

	return -1;
}

static int serve(int sk)
{
	RpcRequest *req;
	int ret;
	unsigned char dbuf[MAX_MSG_ONSTACK];
	libct_session_t ses;

	ses = libct_session_open_local();
	if (!ses)
		return -1;

	while (1) {
		ret = recv(sk, dbuf, MAX_MSG_ONSTACK, 0);
		if (ret <= 0)
			break;

		req = rpc_request__unpack(NULL, ret, dbuf);
		if (!req) {
			ret = -1;
			break;
		}

		ret = serve_req(sk, ses, req);
		rpc_request__free_unpacked(req, NULL);

		if (ret < 0)
			break;
	}

	libct_session_close(ses);
	return ret;
}

int main(int argc, char **argv)
{
	int sk;
	struct sockaddr_un addr;
	socklen_t alen;

	sk = socket(PF_UNIX, SOCK_SEQPACKET, 0);
	if (sk == -1)
		goto err;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, argv[1], sizeof(addr.sun_path));
	alen = strlen(addr.sun_path) + sizeof(addr.sun_family);

	unlink(addr.sun_path);
	if (bind(sk, (struct sockaddr *)&addr, alen))
		goto err;

	if (listen(sk, 16))
		goto err;

	signal(SIGCHLD, SIG_IGN); /* auto-kill zombies */

	while (1) {
		int ask;

		alen = sizeof(addr);
		ask = accept(sk, (struct sockaddr *)&addr, &alen);
		if (ask < 0)
			continue;

		if (fork() == 0) {
			int ret;

			ret = serve(ask);
			if (ret < 0)
				ret = -ret;

			close(ask);
			exit(ret);
		}

		close(ask);
	}

err:
	return 1;
}
