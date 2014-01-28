/* cgmanager-proxy
 *
 * Copyright © 2013 Stphane Graber
 * Author: Stphane Graber <stgraber@ubuntu.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <frontend.h>

DBusConnection *server_conn;

bool master_running(void)
{
	NihError *err;

	server_conn = nih_dbus_connect(CGMANAGER_DBUS_PATH, NULL);
	if (server_conn) {
		dbus_connection_unref (server_conn);
		return true;
	}
	err = nih_error_get();
	nih_free(err);
	return false;
}

int setup_proxy(void)
{
	bool exists_upper = false, exists_lower = false;

	/*
	 * If /sys/fs/cgroup/cgmanager.lower exists,
	 *    if /sys/fs/cgroup/cgmanager exists, then exit (proxy already running)
	 *    start up, connect to .lower
	 * else
	 *    if /sys/fs/cgroup/cgmanager exists, move it to /sys/fs/cgroup/cgmanager.lower
	 *    start up and connect to .lower
	 */
	server_conn = nih_dbus_connect(CGMANAGER_DBUS_PATH, NULL);
	if (server_conn) {
		exists_upper = true;
		dbus_connection_unref (server_conn);
	}
	server_conn = nih_dbus_connect(CGPROXY_DBUS_PATH, NULL);
	if (server_conn) {
		exists_lower = true;
	}
	if (exists_upper && exists_lower) {
		dbus_connection_unref (server_conn);
		nih_fatal("proxy already running");
		return -1;  // proxy already running
	}
	if (exists_lower)
		// we've got the sock we need, all set.
		return 0;
	if (exists_upper) {
		//move /sys/fs/cgroup/cgmanager to /sys/fs/cgroup/cgmanager.lower
		if (mkdir(CGPROXY_DIR, 0755) < 0 && errno != EEXIST) {
			nih_fatal("failed to create lower sock");
			return -1;
		}
		if (mount(CGMANAGER_DIR, CGPROXY_DIR, "none", MS_MOVE, 0) < 0) {
			nih_fatal("unable to rename the socket");
			return -1;
		}
	}
	server_conn = nih_dbus_connect(CGPROXY_DBUS_PATH, NULL);
	return 0;
}

static int checkmaster = FALSE;

void send_dummy_msg(DBusConnection *conn)
{
	DBusMessage *message = NULL;
	DBusMessageIter iter;
	int a;
	message = dbus_message_new_method_call(dbus_bus_get_unique_name(conn),
			"/org/linuxcontainers/cgmanager",
			"org.linuxcontainers.cgmanager0_0", "Ping");
	dbus_message_set_no_reply(message, TRUE);
	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &a)) {
		nih_error_raise_no_memory ();
		return;
	}
	dbus_connection_send(conn, message, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(message);
}

static DBusMessage *start_dbus_request(const char *method, int *sv)
{
	int optval = 1;

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
		nih_error("%s: Error creating socketpair: %s",
			__func__, strerror(errno));
		return NULL;
	}
	if (setsockopt(sv[1], SOL_SOCKET, SO_PASSCRED, &optval, sizeof(optval)) == -1) {
		nih_error("%s: setsockopt: %s", __func__, strerror(errno));
		goto err;
	}
	if (setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &optval, sizeof(optval)) == -1) {
		nih_error("%s: setsockopt: %s", __func__, strerror(errno));
		goto err;
	}

	return dbus_message_new_method_call(dbus_bus_get_unique_name(server_conn),
			"/org/linuxcontainers/cgmanager",
			"org.linuxcontainers.cgmanager0_0", method);
err:
	close(sv[0]);
	close(sv[1]);
	return NULL;
}

static bool complete_dbus_request(DBusMessage *message,
		int *sv, struct ucred *rcred, struct ucred *vcred)
{
	char buf[1];

	if (!dbus_connection_send(server_conn, message, NULL)) {
		nih_error("%s: failed to send dbus message", __func__);
		dbus_message_unref(message);
		return false;
	}
	dbus_connection_flush(server_conn);
	dbus_message_unref(message);

	if (recv(sv[0], buf, 1, 0) != 1) {
		nih_error("%s: Error getting reply from server over socketpair",
			  __func__);
		return false;
	}
	if (send_creds(sv[0], rcred)) {
		nih_error("%s: Error sending pid over SCM_CREDENTIAL",
			__func__);
		return false;
	}

	if (!vcred) // this request only requires one scm_credential
		return true;

	if (recv(sv[0], buf, 1, 0) != 1) {
		nih_error("%s: Error getting reply from server over socketpair",
			__func__);
		return false;
	}
	if (send_creds(sv[0], vcred)) {
		nih_error("%s: Error sending pid over SCM_CREDENTIAL",
			__func__);
		return false;
	}

	return true;
}

int get_pid_cgroup_main (void *parent, const char *controller,
		struct ucred r, struct ucred v, char **output)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char s[MAXPATHLEN] = { 0 };

	if (!(message = start_dbus_request("GetPidCgroupScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (!dbus_message_iter_append_basic (&iter,
			DBUS_TYPE_STRING,
			&controller)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error_raise_no_memory ();
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, &v)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	// TODO - switch to nih_io_message_recv?
	if (recv(sv[0], s, MAXPATHLEN-1, 0) <= 0)
		nih_error("%s: Error reading result from cgmanager",
			__func__);
	else {
		*output = nih_strdup(parent, s);
		ret = 0;
	}
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int move_pid_main (const char *controller, const char *cgroup,
		struct ucred r, struct ucred v)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char buf[1];

	if (!(message = start_dbus_request("MovePidScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,
				&controller)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error_raise_no_memory ();
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, &v)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], buf, 1, 0) == 1 && *buf == '1')
		ret = 0;
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int create_main (const char *controller, const char *cgroup, struct ucred r,
		 int32_t *existed)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char buf[1];

	if (!(message = start_dbus_request("CreateScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error_raise_no_memory ();
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, NULL)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], buf, 1, 0) == 1 && (*buf == '1' || *buf == '2'))
		ret = 0;
	*existed = *buf == '2' ? 1 : -1;
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int chown_main (const char *controller, const char *cgroup,
		struct ucred r, struct ucred v)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char buf[1];

	if (!(message = start_dbus_request("ChownScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error_raise_no_memory ();
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, &v)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], buf, 1, 0) == 1 && *buf == '1')
		ret = 0;
out:
	close(sv[0]);
	close(sv[1]);
	if (message)
		dbus_message_unref(message);
	return ret;
}

int get_value_main (void *parent, const char *controller, const char *req_cgroup,
		 const char *key, struct ucred r, char **value)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char output[MAXPATHLEN] = { 0 };

	if (!(message = start_dbus_request("GetValueScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &req_cgroup)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error_raise_no_memory ();
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, NULL)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], output, MAXPATHLEN, 0) <= 0) {
		nih_error("%s: Failed reading string from cgmanager: %s",
			__func__, strerror(errno));
	} else {
		*value = nih_strdup(parent, output);
		ret = 0;
	}
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int set_value_main (const char *controller, const char *req_cgroup,
		 const char *key, const char *value, struct ucred r)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char buf[1];

	if (!(message = start_dbus_request("SetValueScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &req_cgroup)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &key)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &value)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error_raise_no_memory ();
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, NULL)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], buf, 1, 0) == 1 && *buf == '1')
		ret = 0;
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int remove_main (const char *controller, const char *cgroup, struct ucred r,
		 int recursive, int32_t *existed)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	char buf[1];

	if (!(message = start_dbus_request("RemoveScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &recursive)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error_raise_no_memory ();
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, NULL)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}

	if (recv(sv[0], buf, 1, 0) == 1 && (*buf == '1' || *buf == '2'))
		ret = 0;
	*existed = *buf == '2' ? 1 : -1;
out:
	close(sv[0]);
	close(sv[1]);
	return ret;
}

int get_tasks_main (void *parent, const char *controller, const char *cgroup,
		    struct ucred r, int32_t **pids)
{
	DBusMessage *message;
	DBusMessageIter iter;
	int sv[2], ret = -1;
	uint32_t nrpids;
	struct ucred tcred;
	int i;

	if (!(message = start_dbus_request("GetTasksScm", sv))) {
		nih_error("%s: error starting dbus request", __func__);
		return -1;
	}

	dbus_message_iter_init_append(message, &iter);
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &controller)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &cgroup)) {
		nih_error_raise_no_memory ();
		goto out;
	}
	if (! dbus_message_iter_append_basic (&iter, DBUS_TYPE_UNIX_FD, &sv[1])) {
		nih_error_raise_no_memory ();
		goto out;
	}

	if (!complete_dbus_request(message, sv, &r, NULL)) {
		nih_error("%s: error completing dbus request", __func__);
		goto out;
	}
	if (recv(sv[0], &nrpids, sizeof(uint32_t), 0) != sizeof(uint32_t))
		goto out;
	if (nrpids == 0) {
		ret = 0;
		goto out;
	}

	*pids = nih_alloc(parent, nrpids * sizeof(uint32_t));
	for (i=0; i<nrpids; i++) {
		get_scm_creds_sync(sv[0], &tcred);
		if (tcred.pid == -1) {
			nih_error("%s: Failed getting pid from server",
				__func__);
			goto out;
		}
		(*pids)[i] = tcred.pid;
	}
	ret = nrpids;
out:
	close(sv[0]);
	close(sv[1]);
	if (message)
		dbus_message_unref(message);
	return ret;
}

/**
 * options:
 *
 * Command-line options accepted by this program.
 **/
static NihOption options[] = {
	{ 0, "daemon", N_("Detach and run in the background"),
	  NULL, NULL, &daemonise, NULL },
	{ 0, "check-master", N_("Check whether cgmanager is running"),
	  NULL, NULL, &checkmaster, NULL },

	NIH_OPTION_LAST
};

int
main (int argc, char *argv[])
{
	char **		args;
	int		ret;
	DBusServer *	server;
	struct stat sb;

	nih_main_init (argv[0]);

	nih_option_set_synopsis (_("Control group proxy"));
	nih_option_set_help (_("The cgroup manager proxy"));

	if (geteuid() != 0) {
		nih_error(_("Cgmanager proxy must be run as root"));
		exit(1);
	}

	args = nih_option_parser (NULL, argc, argv, options, FALSE);
	if (! args)
		exit (1);

	/*
	 * If we are called with checkmaster, then only check whether
	 * cgmanager is running.  This is used by the init script to
	 * determine whether to run cgmanager or cgproxy
	 */
	if (checkmaster) {
		if (master_running())
			exit(0);
		exit(1);
	}
	if (setup_proxy() < 0) {
		nih_fatal ("Failed to set up as proxy");
		exit(1);
	}

	/* Setup the DBus server */
	server = nih_dbus_server ( CGMANAGER_DBUS_PATH, client_connect,
			client_disconnect);
	nih_assert (server != NULL);

	if (stat("/proc/self/ns/pid", &sb) == 0) {
		mypidns = read_pid_ns_link(getpid());
		setns_pid_supported = true;
	}

	if (stat("/proc/self/ns/user", &sb) == 0) {
		myuserns = read_user_ns_link(getpid());
		setns_user_supported = true;
	}

	/* Become daemon */
	if (daemonise) {
		if (nih_main_daemonise () < 0) {
			NihError *err;

			err = nih_error_get ();
			nih_fatal ("%s: %s", _("Unable to become daemon"),
				   err->message);
			nih_free (err);

			exit (1);
		}
	}

	send_dummy_msg(server_conn);

	ret = nih_main_loop ();

	/* Destroy any PID file we may have created */
	if (daemonise) {
		nih_main_unlink_pidfile();
	}

	return ret;
}
