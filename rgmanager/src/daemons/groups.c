/*
  Copyright Red Hat, Inc. 2002-2003
  Copyright Mission Critical Linux, Inc. 2000

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; either version 2, or (at your option) any
  later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING.  If not, write to the
  Free Software Foundation, Inc.,  675 Mass Ave, Cambridge, 
  MA 02139, USA.
*/
//#define DEBUG
#include <platform.h>
#include <resgroup.h>
#include <vf.h>
#include <message.h>
#include <ccs.h>
#include <clulog.h>
#include <members.h>
#include <list.h>
#include <reslist.h>
#include <assert.h>

#define cm_svccount cm_pad[0] /* Theses are uint8_t size */
#define cm_svcexcl  cm_pad[1]


static int config_version = 0;
static resource_t *_resources = NULL;
static resource_rule_t *_rules = NULL;
static resource_node_t *_tree = NULL;
static fod_t *_domains = NULL;

pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t resource_lock = PTHREAD_RWLOCK_INITIALIZER;


struct status_arg {
	msgctx_t *ctx;
	int fast;
};


/**
   See if a given node ID should start a resource, given cluster membership

   @see node_should_start
 */
int
node_should_start_safe(uint64_t nodeid, cluster_member_list_t *membership,
		       char *rg_name)
{
	int ret;

	pthread_rwlock_rdlock(&resource_lock);
	ret = node_should_start(nodeid, membership, rg_name, &_domains);
	pthread_rwlock_unlock(&resource_lock);

	return ret;
}


int
count_resource_groups(cluster_member_list_t *ml)
{
#if 0
	resource_t *res;
	char *rgname, *val;
	int x;
	rg_state_t st;
	void *lockp;
	cman_node_t *mp;

	for (x = 0; x < ml->cml_count; x++) {
		ml->cml_members[x].cm_svccount = 0;
		ml->cml_members[x].cm_svcexcl = 0;
	}

	pthread_rwlock_rdlock(&resource_lock);

	list_do(&_resources, res) {
		if (res->r_rule->rr_root == 0)
			continue;

		rgname = res->r_attrs[0].ra_value;

		if (rg_lock(rgname, &lockp) < 0) {
			clulog(LOG_ERR, "#XX: Unable to obtain cluster "
			       "lock @ %s:%d: %s\n", __FILE__, __LINE__,
			       strerror(errno));
			continue;
		}

		if (get_rg_state(rgname, &st) < 0) {
			clulog(LOG_ERR, "#34: Cannot get status "
			       "for service %s\n", rgname);
			rg_unlock(rgname, lockp);
			continue;
		}

		rg_unlock(rgname, lockp);

		if (st.rs_state != RG_STATE_STARTED &&
		     st.rs_state != RG_STATE_STARTING)
			continue;

		mp = memb_id_to_p(ml, st.rs_owner);
		if (!mp)
			continue;

		++mp->cm_svccount;

		val = res_attr_value(res, "exclusive");
		if (val && ((!strcmp(val, "yes") ||
				     (atoi(val)>0))) ) {
			++mp->cm_svcexcl;
		}

	} while (!list_done(&_resources, res));

	pthread_rwlock_unlock(&resource_lock);
#endif
	return 0;
}


/**
   Find the best target node for a service *besides* the current service
   owner.  Takes into account:

   - Failover domain (ordering / restricted policy)
   - Exclusive service policy
 */
uint64_t
best_target_node(cluster_member_list_t *allowed, uint64_t owner,
		 char *rg_name, int lock)
{
	int x;
	int highscore = 1;
	int score;
	uint64_t highnode = owner, nodeid;
	char *val;
	resource_t *res;
	int exclusive;

	if (lock)
		pthread_rwlock_rdlock(&resource_lock);
	count_resource_groups(allowed);
	if (lock)
		pthread_rwlock_unlock(&resource_lock);

	for (x=0; x < allowed->cml_count; x++) {
		if (!allowed->cml_members[x].cn_member)
			continue;

		nodeid = allowed->cml_members[x].cn_nodeid;

		/* Don't allow trying a restart just yet */
		if (owner != 0 && nodeid == owner)
			continue;
		
		if (lock)
			pthread_rwlock_rdlock(&resource_lock);
		score = node_should_start(nodeid, allowed, rg_name, &_domains);
		if (!score) { /* Illegal -- failover domain constraint */
			if (lock)
				pthread_rwlock_unlock(&resource_lock);
			continue;
		}

		/* Add 2 to score if it's an exclusive service and nodeid
		   isn't running any services currently.  Set score to 0 if
		   it's an exclusive service and the target node already
		   is running a service. */
		res = find_root_by_ref(&_resources, rg_name);
		val = res_attr_value(res, "exclusive");
		exclusive = val && ((!strcmp(val, "yes") || (atoi(val)>0)));

		if (lock)
			pthread_rwlock_unlock(&resource_lock);

		if (exclusive) {

			if (0) {//(allowed->cml_members[x].cm_svccount > 0) {
				/* Definitely not this guy */
				continue;
			} else {
				score += 2;
			}
		} else if (0) { //(allowed->cml_members[x].cm_svcexcl) {
			/* This guy has an exclusive resource group.
			   Can't relocate / failover to him. */
			continue;
		}

		if (score < highscore)
			continue;

		highnode = nodeid;
		highscore = score;
	}

	return highnode;
}


/**
  Start or failback a resource group: if it's not running, start it.
  If it is running and we're a better member to run it, then ask for
  it.
 */
void
consider_start(resource_node_t *node, char *svcName, rg_state_t *svcStatus,
	       cluster_member_list_t *membership)
{
	char *val;
	cman_node_t *mp;
	int autostart, exclusive;
	void *lockp;

	mp = memb_id_to_p(membership, my_id());
	assert(mp);

	/*
	 * Service must be not be running elsewhere to consider for a
	 * local start.
	 */
	if (svcStatus->rs_state == RG_STATE_STARTED &&
	    svcStatus->rs_owner == mp->cn_nodeid)
		return;

	if (svcStatus->rs_state == RG_STATE_DISABLED)
		return;

	/* Stopped, and hasn't been started yet.  See if
	   autostart is disabled.  If it is, leave it stopped */
	if (svcStatus->rs_state == RG_STATE_STOPPED &&
		    svcStatus->rs_transition == 0) {
		val = res_attr_value(node->rn_resource, "autostart");
		autostart = !(val && ((!strcmp(val, "no") ||
				     (atoi(val)==0))));
		if (!autostart) {
			/*
			clulog(LOG_DEBUG,
			       "Skipping RG %s: Autostart disabled\n",
			       svcName);
			 */
			/*
			   Mark non-autostart services as disabled to avoid
			   confusion!
			 */
			if (rg_lock(svcName, &lockp) < 0) {
				clulog(LOG_ERR, "#XX: Unable to obtain cluster "
				       "lock @ %s:%d: %s\n", __FILE__, __LINE__,
				       strerror(errno));
				return;
			}

			if (get_rg_state(svcName, svcStatus) != 0) {
				clulog(LOG_ERR, "#34: Cannot get status "
				       "for service %s\n", svcName);
				rg_unlock(svcName, lockp);
				return;
			}

			if (svcStatus->rs_transition == 0 &&
			    svcStatus->rs_state == RG_STATE_STOPPED) {
				svcStatus->rs_state = RG_STATE_DISABLED;
				set_rg_state(svcName, svcStatus);
			}

			rg_unlock(svcName, lockp);

			return;
		}
	}

	val = res_attr_value(node->rn_resource, "exclusive");
	exclusive = val && ((!strcmp(val, "yes") || (atoi(val)>0)));

	if (0) { //(exclusive && mp->cm_svccount) {
		clulog(LOG_INFO,
		       "Skipping RG %s: Exclusive and I am running services\n",
		       svcName);
		return;
	}

	/*
	   Don't start other services if I'm running an exclusive
	   service.
	 */
	if (0) { //(mp->cm_svcexcl) {
		clulog(LOG_INFO,
		       "Skipping RG %s: I am running an exclusive service\n",
		       svcName);
		return;
	}

	/*
	 * Start any stopped services, or started services
	 * that are owned by a down node.
	 */
	if (node_should_start(mp->cn_nodeid, membership, svcName, &_domains) ==
	    FOD_BEST)
		rt_enqueue_request(svcName, RG_START, NULL, 0, mp->cn_nodeid,
				   0, 0);
}


void
consider_relocate(char *svcName, rg_state_t *svcStatus, uint64_t nodeid,
		  cluster_member_list_t *membership)
{
	int a, b;

	/*
	   Service must be running locally in order to consider for
	   a relocate
	 */
	if (svcStatus->rs_state != RG_STATE_STARTED ||
	    svcStatus->rs_owner != my_id())
		return;

	/*
	 * Send the resource group to a node if it's got a higher prio
	 * to run the resource group.
	 */
#if 0
	if (best_target_node(membership, my_id(), svcName, 0) !=
	    nodeid) {
		return;
	}
#endif
	a = node_should_start(nodeid, membership, svcName, &_domains);
	b = node_should_start(my_id(), membership, svcName, &_domains);

	if (a <= b)
		return;

	clulog(LOG_DEBUG, "Relocating group %s to better node %s\n",
	       svcName,
	       memb_id_to_name(membership, nodeid));

	rt_enqueue_request(svcName, RG_RELOCATE, NULL, 0, nodeid, 0, 0);
}


/**
 * Called to decide what services to start locally during a node_event.
 * Originally a part of node_event, it is now its own function to cut down
 * on the length of node_event.
 * 
 * @see			node_event
 */
int
eval_groups(int local, uint64_t nodeid, int nodeStatus)
{
	void *lockp = NULL;
	char *svcName, *nodeName;
	resource_node_t *node;
	rg_state_t svcStatus;
	cluster_member_list_t *membership;
	int ret;

	if (rg_locked()) {
		clulog(LOG_NOTICE,
			"Resource groups locked; not evaluating\n");
		return -EAGAIN;
	}

	membership = member_list();

	pthread_rwlock_rdlock(&resource_lock);

	/* Requires read lock */
	count_resource_groups(membership);

	list_do(&_tree, node) {

		if (node->rn_resource->r_rule->rr_root == 0)
			continue;

		svcName = node->rn_resource->r_attrs->ra_value;

		/*
		 * Lock the service information and get the current service
		 * status.
		 */
		if ((ret = rg_lock(svcName, &lockp)) < 0) {
			clulog(LOG_ERR,
			       "#33: Unable to obtain cluster lock: %s\n",
			       strerror(-ret));
			pthread_rwlock_unlock(&resource_lock);
			free_member_list(membership);
			return ret;
		}
		
		if (get_rg_state(svcName, &svcStatus) != 0) {
			clulog(LOG_ERR,
			       "#34: Cannot get status for service %s\n",
			       svcName);
			rg_unlock(svcName, lockp);
			continue;
		}

		rg_unlock(svcName, lockp);

		if (svcStatus.rs_owner == 0)
			nodeName = "none";
		else
			nodeName = memb_id_to_name(membership,
						   svcStatus.rs_owner);

		/* Disabled/failed/in recovery?  Do nothing */
		if ((svcStatus.rs_state == RG_STATE_DISABLED) ||
		    (svcStatus.rs_state == RG_STATE_FAILED) ||
		    (svcStatus.rs_state == RG_STATE_RECOVER)) {
			continue;
		}

		clulog(LOG_DEBUG, "Evaluating RG %s, state %s, owner "
		       "%s\n", svcName,
		       rg_state_str(svcStatus.rs_state),
		       nodeName);

		if ((local && nodeStatus) ||
		    svcStatus.rs_state == RG_STATE_STOPPED) {

			consider_start(node, svcName, &svcStatus, membership);

		} else if (!local && !nodeStatus) {

			/*
			 * Start any stopped services, or started services
			 * that are owned by a down node.
			 */
			consider_start(node, svcName, &svcStatus, membership);

			/*
			 * TODO
			 * Mark a service as 'stopped' if no members in its
			 * restricted fail-over domain are running.
			 */
		} else {
			/* Send to the node if that ndoe is a better
			   owner for this service */
			consider_relocate(svcName, &svcStatus, nodeid,
					  membership);
		}

	} while (!list_done(&_tree, node));

	pthread_rwlock_unlock(&resource_lock);
	free_member_list(membership);

	clulog(LOG_INFO, "Event (%d:%d:%d) Processed\n", local,
	       (int)nodeid, nodeStatus);

	return 0;
}


/**
   Perform an operation on a resource group.  That is, walk down the
   tree for that resource group, performing the given operation on
   all children in the necessary order.

   XXX Needs to handle more return codes to be more OCF compliant

   @param groupname	Resource group to operate on
   @param op		Operation to perform
   @return 		0 on success, 1 on failure/error.
 */
int
group_op(char *groupname, int op)
{
	resource_t *res;
	int ret = -1;

	pthread_rwlock_rdlock(&resource_lock);
	/* XXX get group from somewhere else */
	res = find_root_by_ref(&_resources, groupname);
	if (!res) {
		pthread_rwlock_unlock(&resource_lock);
		return -1;
	}

	switch (op) {
	case RG_START:
		ret = res_start(&_tree, res, NULL);
		break;
	case RG_STOP:
		ret = res_stop(&_tree, res, NULL);
		break;
	case RG_STATUS:
		ret = res_status(&_tree, res, NULL);
		break;
	case RG_CONDSTOP:
		ret = res_condstop(&_tree, res, NULL);
		break;
	case RG_CONDSTART:
		ret = res_condstart(&_tree, res, NULL);
		break;
	}
	pthread_rwlock_unlock(&resource_lock);

	/*
	   Do NOT return error codes if we failed to stop for one of these
	   reasons.  It didn't start, either, so it's safe to assume that
	   if the program wasn't installed, there's nothing to tear down.
	 */
	if (op == RG_STOP) {
		switch(ret) {
		case OCF_RA_SUCCESS:
		case OCF_RA_NOT_INSTALLED:
		case OCF_RA_NOT_CONFIGURED:
			ret = 0;
			break;
		default:
			ret = 1;
			break;
		}
	}

	return ret;
}


/**
   Gets an attribute of a resource group.

   @param groupname	Name of group
   @param property	Name of property to check for
   @param ret		Preallocated return buffer
   @param len		Length of buffer pointed to by ret
   @return		0 on success, -1 on failure.
 */
int
group_property(char *groupname, char *property, char *ret, size_t len)
{
	resource_t *res = NULL;
	int x = 0;

	pthread_rwlock_rdlock(&resource_lock);
	res = find_root_by_ref(&_resources, groupname);
	if (!res) {
		pthread_rwlock_unlock(&resource_lock);
		return -1;
	}

	for (; res->r_attrs[x].ra_name; x++) {
		if (strcasecmp(res->r_attrs[x].ra_name, property))
			continue;
		strncpy(ret, res->r_attrs[x].ra_value, len);
		pthread_rwlock_unlock(&resource_lock);
		return 0;
	}
	pthread_rwlock_unlock(&resource_lock);

	return -1;
}


/**
  Send the state of a resource group to a given file descriptor.

  @param fd		File descriptor to send state to
  @param rgname		Resource group name whose state we want to send.
  @see send_rg_states
 */
int get_rg_state_local(char *, rg_state_t *);
void
send_rg_state(msgctx_t *ctx, char *rgname, int fast)
{
	rg_state_msg_t msg, *msgp = &msg;
	void *lockp;

	msgp->rsm_hdr.gh_magic = GENERIC_HDR_MAGIC;
	msgp->rsm_hdr.gh_length = sizeof(msg);
	msgp->rsm_hdr.gh_command = RG_STATUS;

	/* try fast read -- only if it fails and fast is not 
	   specified should we do the full locked read */
	if (get_rg_state_local(rgname, &msgp->rsm_state) != 0 &&
	    !fast) {
		if (rg_lock(rgname, &lockp) < 0)
			return;
		if (get_rg_state(rgname, &msgp->rsm_state) < 0) {
			rg_unlock(rgname, lockp);
			return;
		}
		rg_unlock(rgname, lockp);
	}

	swab_rg_state_msg_t(msgp);

	if (msg_send(ctx, msgp, sizeof(msg)) < 0)
		perror("msg_send");
}


/**
  Send status from a thread because we don't want rgmanager's
  main thread to block in the case of DLM issues
 */
static void *
status_check_thread(void *arg)
{
	msgctx_t *ctx = ((struct status_arg *)arg)->ctx;
	int fast = ((struct status_arg *)arg)->fast;
	resource_t *res;
	generic_msg_hdr hdr;

	free(arg);

	pthread_rwlock_rdlock(&resource_lock);

	list_do(&_resources, res) {
		if (res->r_rule->rr_root == 0)
			continue;

		send_rg_state(ctx, res->r_attrs[0].ra_value, fast);
	} while (!list_done(&_resources, res));

	pthread_rwlock_unlock(&resource_lock);

	msg_send_simple(ctx, RG_SUCCESS, 0, 0);

	/* XXX wait for client to tell us it's done; I don't know why
	   this is needed when doing fast I/O, but it is. */
	msg_receive(ctx, &hdr, sizeof(hdr), 10);

	msg_close(ctx);

	return NULL;
}


/**
  Send all resource group states to a file descriptor

  @param fd		File descriptor to send states to.
  @return		0
 */
int
send_rg_states(msgctx_t *ctx, int fast)
{
	struct status_arg *arg;
	pthread_t newthread;
	pthread_attr_t attrs;

	arg = malloc(sizeof(struct status_arg));
	if (!arg) {
		msg_send_simple(ctx, RG_FAIL, 0, 0);
		return -1;
	}

	arg->ctx = ctx;
	arg->fast = fast;

        pthread_attr_init(&attrs);
        pthread_attr_setinheritsched(&attrs, PTHREAD_INHERIT_SCHED);
        pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attrs, 65535);

	pthread_create(&newthread, &attrs, status_check_thread, arg);
        pthread_attr_destroy(&attrs);

	return 0;
}


int
svc_exists(char *svcname)
{
	resource_t *res;
	int ret = 0;

	pthread_rwlock_rdlock(&resource_lock);

	list_do(&_resources, res) {
		if (res->r_rule->rr_root == 0)
			continue;

		if (strcmp(res->r_attrs[0].ra_value, 
			   svcname) == 0) {
			ret = 1;
			break;
		}
	} while (!list_done(&_resources, res));

	pthread_rwlock_unlock(&resource_lock);

	return ret;
}


void
rg_doall(int request, int block, char *debugfmt)
{
	resource_node_t *curr;
	char *name;
	rg_state_t svcblk;

	pthread_rwlock_rdlock(&resource_lock);
	list_do(&_tree, curr) {

		if (curr->rn_resource->r_rule->rr_root == 0)
			continue;

		/* Group name */
		name = curr->rn_resource->r_attrs->ra_value;

		if (debugfmt)
			clulog(LOG_DEBUG, debugfmt, name);

		/* Optimization: Don't bother even queueing the request
		   during the exit case if we don't own it */
		if (request == RG_STOP_EXITING) {
			if (get_rg_state_local(name, &svcblk) < 0)
				continue;

			/* Always run stop if we're the owner, regardless
			   of state; otherwise, don't run stop */
			if (svcblk.rs_owner != my_id())
				continue;
		}

		rt_enqueue_request(name, request, NULL, 0,
				   0, 0, 0);
	} while (!list_done(&_tree, curr));

	pthread_rwlock_unlock(&resource_lock);

	/* XXX during shutdown, if we're doing a simultaenous shutdown,
	   this will cause this rgmanager to hang waiting for all the
	   other rgmanagers to complete. */
	if (block) 
		rg_wait_threads();
}


/**
  Stop changed resources.
 */
void *
q_status_checks(void *arg)
{
	resource_node_t *curr;
	char *name;
	rg_state_t svcblk;
	void *lockp;

	pthread_rwlock_rdlock(&resource_lock);
	list_do(&_tree, curr) {

		if (curr->rn_resource->r_rule->rr_root == 0)
			continue;

		/* Group name */
		name = curr->rn_resource->r_attrs->ra_value;

		/* Local check - no one will make us take a service */
		if (get_rg_state_local(name, &svcblk) < 0) {
			continue;
		}

		if (svcblk.rs_owner != my_id() ||
		    svcblk.rs_state != RG_STATE_STARTED)
			continue;

		rt_enqueue_request(name, RG_STATUS,
				   NULL, 0, 0, 0, 0);

	} while (!list_done(&_tree, curr));

	pthread_rwlock_unlock(&resource_lock);

	return NULL;
}


void
do_status_checks(void)
{
	pthread_attr_t attrs;
	pthread_t newthread;

        pthread_attr_init(&attrs);
        pthread_attr_setinheritsched(&attrs, PTHREAD_INHERIT_SCHED);
        pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attrs, 65535);

	pthread_create(&newthread, &attrs, q_status_checks, NULL);
        pthread_attr_destroy(&attrs);
}


/**
  Stop changed resources.
 */
void
do_condstops(void)
{
	resource_node_t *curr;
	char *name;
	rg_state_t svcblk;
	int need_kill;
	void *lockp;

	clulog(LOG_INFO, "Stopping changed resources.\n");

	pthread_rwlock_rdlock(&resource_lock);
	list_do(&_tree, curr) {

		if (curr->rn_resource->r_rule->rr_root == 0)
			continue;

		/* Group name */
		name = curr->rn_resource->r_attrs->ra_value;

		/* If we're not running it, no need to CONDSTOP */
		if (get_rg_state_local(name, &svcblk) < 0) {
			continue;
		}

		if (svcblk.rs_owner != my_id())
			continue;

		/* Set state to uninitialized if we're killing a RG */
		need_kill = 0;
		if (curr->rn_resource->r_flags & RF_NEEDSTOP) {
			need_kill = 1;
			clulog(LOG_DEBUG, "Removing %s\n", name);
		}

		rt_enqueue_request(name, need_kill ? RG_DISABLE : RG_CONDSTOP,
				   NULL, 0, 0, 0, 0);

	} while (!list_done(&_tree, curr));

	pthread_rwlock_unlock(&resource_lock);
	rg_wait_threads();
}


/**
  Start changed resources.
 */
void
do_condstarts(void)
{
	resource_node_t *curr;
	char *name, *val;
	rg_state_t svcblk;
	int need_init, new_groups = 0, autostart;
	void *lockp;

	clulog(LOG_INFO, "Starting changed resources.\n");

	/* Pass 1: Start any normally changed resources */
	pthread_rwlock_rdlock(&resource_lock);
	list_do(&_tree, curr) {

		if (curr->rn_resource->r_rule->rr_root == 0)
			continue;

		/* Group name */
		name = curr->rn_resource->r_attrs->ra_value;

		/* New RG.  We'll need to initialize it. */
		need_init = 0;
		if (curr->rn_resource->r_flags & RF_NEEDSTART)
			need_init = 1;

		if (get_rg_state_local(name, &svcblk) < 0) {
			continue;
		}

		if (!need_init && svcblk.rs_owner != my_id()) {
			continue;
		}

		if (need_init) {
			++new_groups;
			clulog(LOG_DEBUG, "Initializing %s\n", name);
		}

		rt_enqueue_request(name, need_init ? RG_INIT : RG_CONDSTART,
				   NULL, 0, 0, 0, 0);

	} while (!list_done(&_tree, curr));

	pthread_rwlock_unlock(&resource_lock);
	rg_wait_threads();

	if (!new_groups)
		return;

	/* Pass 2: Tag all new resource groups as stopped */
	pthread_rwlock_rdlock(&resource_lock);
	list_do(&_tree, curr) {

		if (curr->rn_resource->r_rule->rr_root == 0)
			continue;

		/* Group name */
		name = curr->rn_resource->r_attrs->ra_value;

		/* New RG.  We'll need to initialize it. */
		if (!(curr->rn_resource->r_flags & RF_NEEDSTART))
			continue;

		if (rg_lock(name, &lockp) != 0)
			continue;

		if (get_rg_state(name, &svcblk) < 0) {
			rg_unlock(name, lockp);
			continue;
		}

		/* If it is a replacement of an old RG, it will
		   be in the DISABLED state, which will prevent it
		   from restarting.  That's bad.  However, if it's
		   a truly new service, it will be in the UNINITIALIZED
		   state, which will be caught by eval_groups. */
		if (svcblk.rs_state != RG_STATE_DISABLED) {
			rg_unlock(name, lockp);
			continue;
		}

		/* Set it up for an auto-start */
		val = res_attr_value(curr->rn_resource, "autostart");
		autostart = !(val && ((!strcmp(val, "no") ||
				     (atoi(val)==0))));
		if (autostart)
			svcblk.rs_state = RG_STATE_STOPPED;
		else
			svcblk.rs_state = RG_STATE_DISABLED;

		set_rg_state(name, &svcblk);

		rg_unlock(name, lockp);

	} while (!list_done(&_tree, curr));
	pthread_rwlock_unlock(&resource_lock);

	/* Pass 3: See if we should start new resource groups */
	eval_groups(1, my_id(), 1);
}


int
check_config_update(void)
{
	int newver = 0, fd, ret = 0;
	char *val = NULL;

       	fd = ccs_lock();
	if (fd == -1) {
		return 0;
	}

	if (ccs_get(fd, "/cluster/@config_version", &val) == 0) {
		newver = atoi(val);
	}

	if (val)
		free(val);

	pthread_mutex_lock(&config_mutex);
	if (newver && newver != config_version)
		ret = 1;
	pthread_mutex_unlock(&config_mutex);
	ccs_unlock(fd);

	return ret;
}


/**
  Initialize resource groups.  This reads all the resource groups from 
  CCS, builds the tree, etc.  Ideally, we'll have a similar function 
  performing deltas on the two trees so that we can fully support online
  resource group modification.
 */
int
init_resource_groups(int reconfigure)
{
	int fd, x;

	resource_t *reslist = NULL, *res;
	resource_rule_t *rulelist = NULL, *rule;
	resource_node_t *tree = NULL;
	fod_t *domains = NULL, *fod;
	char *val;

	if (reconfigure)
		clulog(LOG_NOTICE, "Reconfiguring\n");
	clulog(LOG_INFO, "Loading Service Data\n");
	clulog(LOG_DEBUG, "Loading Resource Rules\n");
	if (load_resource_rules(RESOURCE_ROOTDIR, &rulelist) != 0) {
		return -1;
	}
	x = 0;
	list_do(&rulelist, rule) { ++x; } while (!list_done(&rulelist, rule));
	clulog(LOG_DEBUG, "%d rules loaded\n", x);

       	fd = ccs_lock();
	if (fd == -1) {
		clulog(LOG_CRIT, "#5: Couldn't connect to ccsd!\n");
		return -1;
	}

	if (ccs_get(fd, "/cluster/@config_version", &val) == 0) {
		pthread_mutex_lock(&config_mutex);
		config_version = atoi(val);
		pthread_mutex_unlock(&config_mutex);
	}

	clulog(LOG_DEBUG, "Building Resource Trees\n");
	/* About to update the entire resource tree... */
	if (load_resources(fd, &reslist, &rulelist) != 0) {
		clulog(LOG_CRIT, "#6: Error loading services\n");
		destroy_resources(&reslist);
		destroy_resource_rules(&rulelist);
		ccs_unlock(fd);
		return -1;
	}

	if (build_resource_tree(fd, &tree, &rulelist, &reslist) != 0) {
		clulog(LOG_CRIT, "#7: Error building resource tree\n");
		destroy_resource_tree(&tree);
		destroy_resources(&reslist);
		destroy_resource_rules(&rulelist);
		ccs_unlock(fd);
		return -1;
	}

	x = 0;
	list_do(&reslist, res) { ++x; } while (!list_done(&reslist, res));
	clulog(LOG_DEBUG, "%d resources defined\n", x);

	clulog(LOG_DEBUG, "Loading Failover Domains\n");
	construct_domains(fd, &domains);
	x = 0;
	list_do(&domains, fod) { ++x; } while (!list_done(&domains, fod));
	clulog(LOG_DEBUG, "%d domains defined\n", x);

	/* Reconfiguration done */
	ccs_unlock(fd);

	if (reconfigure) {
		/* Calc tree deltas */
		pthread_rwlock_wrlock(&resource_lock);
		resource_delta(&_resources, &reslist);
		resource_tree_delta(&_tree, &tree);
		pthread_rwlock_unlock(&resource_lock);

		do_condstops();
	}

	/* Swap in the new configuration */
	pthread_rwlock_wrlock(&resource_lock);
	if (_tree)
		destroy_resource_tree(&_tree);
	_tree = tree;
	if (_resources)
		destroy_resources(&_resources);
	_resources = reslist;
	if (_rules)
		destroy_resource_rules(&_rules);
	_rules = rulelist;
	if (_domains)
		deconstruct_domains(&_domains);
	_domains = domains;
	pthread_rwlock_unlock(&resource_lock);

	if (reconfigure) {
		/* Switch to read lock and do the up-half of the
		   reconfig request */
		clulog(LOG_INFO, "Restarting changed resources.\n");
		do_condstarts();
	} else {
		/* Do initial stop-before-start */
		clulog(LOG_INFO, "Initializing Services\n");
		rg_doall(RG_INIT, 1, "Initializing %s\n");
		clulog(LOG_INFO, "Services Initialized\n");
		rg_set_initialized();
	}

	return 0;
}


void
get_recovery_policy(char *rg_name, char *buf, size_t buflen)
{
	resource_t *res;
	char *val;

	pthread_rwlock_rdlock(&resource_lock);

	strncpy(buf, "restart", buflen);
	res = find_root_by_ref(&_resources, rg_name);
	if (res) {
		val = res_attr_value(res, "recovery");
		if (val) {
			strncpy(buf, val, buflen);
		}
	}

	pthread_rwlock_unlock(&resource_lock);
}


void
kill_resource_groups(void)
{
	pthread_rwlock_wrlock(&resource_lock);

	destroy_resource_tree(&_tree);
	destroy_resources(&_resources);
	destroy_resource_rules(&_rules);
	deconstruct_domains(&_domains);

	pthread_rwlock_unlock(&resource_lock);
}