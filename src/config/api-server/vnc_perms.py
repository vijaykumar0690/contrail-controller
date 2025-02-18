#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#
import sys
from cfgm_common import jsonutils as json
import string
import uuid
from provision_defaults import *
from cfgm_common.exceptions import *
from pysandesh.gen_py.sandesh.ttypes import SandeshLevel

class VncPermissions(object):

    mode_str = {PERMS_R: 'read', PERMS_W: 'write', PERMS_X: 'link'}

    def __init__(self, server_mgr, args):
        self._server_mgr = server_mgr
    # end __init__

    @property
    def _multi_tenancy(self):
        return self._server_mgr._args.multi_tenancy
    # end

    @property
    def _rbac(self):
        return self._server_mgr._args.multi_tenancy_with_rbac
    # end

    def validate_user_visible_perm(self, id_perms, is_admin):
        return id_perms.get('user_visible', True) is not False or is_admin
    # end

    def validate_perms(self, request, uuid, mode=PERMS_R):
        # retrieve object and permissions
        try:
            id_perms = self._server_mgr._db_conn.uuid_to_obj_perms(uuid)
        except NoIdError:
            return (True, '')

        err_msg = (403, 'Permission Denied')

        user, roles = self.get_user_roles(request)
        is_admin = 'admin' in [x.lower() for x in roles]
        if is_admin:
            return (True, '')

        owner = id_perms['permissions']['owner']
        group = id_perms['permissions']['group']
        perms = id_perms['permissions']['owner_access'] << 6 | \
            id_perms['permissions']['group_access'] << 3 | \
            id_perms['permissions']['other_access']

        # check perms
        mask = 0
        if user == owner:
            mask |= 0700
        if group in roles:
            mask |= 0070
        if mask == 0:   # neither user nor group
            mask = 07

        mode_mask = mode | mode << 3 | mode << 6
        ok = is_admin or (mask & perms & mode_mask)

        if ok and mode == PERMS_W:
            ok = self.validate_user_visible_perm(id_perms, is_admin)

        return (True, '') if ok else (False, err_msg)
    # end validate_perms

    def validate_perms_rbac(self, request, obj_uuid, mode=PERMS_R):
        err_msg = (403, 'Permission Denied')

        # retrieve object and permissions
        try:
            perms2 = self._server_mgr._db_conn.uuid_to_obj_perms2(obj_uuid)
        except NoIdError:
            return (False, err_msg)

        user, roles = self.get_user_roles(request)
        is_admin = 'admin' in [x.lower() for x in roles]
        if is_admin:
            return (True, '')

        env = request.headers.environ
        tenant = env.get('HTTP_X_PROJECT_ID', None)
        if tenant is None:
            return (False, err_msg)

        owner = perms2['owner']
        perms = perms2['owner_access'] << 6
        perms |= perms2['global_access']

        # build perms
        mask = 0
        if tenant == owner:
            mask |= 0700

        share = perms2['share']
        tenants = [item['tenant'] for item in share]
        for item in share:
            if tenant == item['tenant']:
                perms = perms | item['tenant_access'] << 3
                mask |= 0070
                break

        if mask == 0:   # neither user nor shared
            mask = 07

        mode_mask = mode | mode << 3 | mode << 6
        ok = is_admin or (mask & perms & mode_mask)

        msg = '%s %s %s admin=%s, mode=%03o mask=%03o perms=%03o, \
            (usr=%s/own=%s/sh=%s)' \
            % ('+++' if ok else '---', self.mode_str[mode], obj_uuid,
               'yes' if is_admin else 'no', mode_mask, mask, perms,
               tenant, owner, tenants)
        self._server_mgr.config_log(msg, level=SandeshLevel.SYS_DEBUG)

        return (True, '') if ok else (False, err_msg)
    # end validate_perms

    # retreive user/role from incoming request
    def get_user_roles(self, request):
        user = []
        env = request.headers.environ
        if 'HTTP_X_USER' in env:
            user = env['HTTP_X_USER']
        roles = []
        if 'HTTP_X_ROLE' in env:
            roles = env['HTTP_X_ROLE'].split(',')
        return (user, roles)
    # end get_user_roles

    # set user/role in object dict from incoming request
    # called from post handler when object is created
    # TODO dsetia - no longer applicable to new RBAC permissions
    def set_user_role(self, request, obj_dict):
        (user, roles) = self.get_user_roles(request)
        if user:
            obj_dict['id_perms']['permissions']['owner'] = user
        if roles:
            obj_dict['id_perms']['permissions']['group'] = roles[0]
    # end set_user_role

    def check_perms_write(self, request, id):
        app = request.environ['bottle.app']
        if app.config.local_auth or self._server_mgr.is_auth_disabled():
            return (True, '')

        if self._rbac:
            return self.validate_perms_rbac(request, id, PERMS_W)
        elif self._multi_tenancy:
            return self.validate_perms(request, id, PERMS_W)
        else:
            return (True, '')
    # end check_perms_write

    def check_perms_read(self, request, id):
        app = request.environ['bottle.app']
        if app.config.local_auth or self._server_mgr.is_auth_disabled():
            return (True, '')

        if self._rbac:
            return self.validate_perms_rbac(request, id, PERMS_R)
        elif self._multi_tenancy:
            return self.validate_perms(request, id, PERMS_R)
        else:
            return (True, '')
    # end check_perms_read

    def check_perms_link(self, request, id):
        app = request.environ['bottle.app']
        if app.config.local_auth or self._server_mgr.is_auth_disabled():
            return (True, '')

        if self._rbac:
            return self.validate_perms_rbac(request, id, PERMS_X)
        elif self._multi_tenancy:
            return self.validate_perms(request, id, PERMS_X)
        else:
            return (True, '')
    # end check_perms_link


