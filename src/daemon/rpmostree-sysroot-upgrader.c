/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <libglnx.h>
#include <systemd/sd-journal.h>
#include "rpmostreed-utils.h"
#include "rpmostree-util.h"

#include "rpmostree-sysroot-upgrader.h"
#include "rpmostree-core.h"
#include "rpmostree-origin.h"
#include "rpmostree-kernel.h"
#include "rpmostree-rpm-util.h"
#include "rpmostree-postprocess.h"
#include "rpmostree-output.h"

#include "ostree-repo.h"

#define RPMOSTREE_TMP_BASE_REF "rpmostree/base/tmp"
#define RPMOSTREE_TMP_ROOTFS_DIR "extensions/rpmostree/commit"

/**
 * SECTION:rpmostree-sysroot-upgrader
 * @title: Simple upgrade class
 * @short_description: Upgrade RPM+OSTree systems
 *
 * The #RpmOstreeSysrootUpgrader class models a `baserefspec` OSTree branch
 * in an origin file, along with a set of layered RPM packages.
 *
 * It also supports the plain-ostree "refspec" model.
 */
typedef struct {
  GObjectClass parent_class;
} RpmOstreeSysrootUpgraderClass;

struct RpmOstreeSysrootUpgrader {
  GObject parent;

  OstreeSysroot *sysroot;
  OstreeRepo *repo;
  char *osname;
  RpmOstreeSysrootUpgraderFlags flags;

  OstreeDeployment *cfg_merge_deployment;
  OstreeDeployment *origin_merge_deployment;
  RpmOstreeOrigin *origin;

  /* Used during tree construction */
  OstreeRepoDevInoCache *devino_cache;
  int tmprootfs_dfd;
  RpmOstreeRefSack *rsack;

  GPtrArray *overlay_packages; /* Finalized list of pkgs to overlay */

  char *base_revision; /* Non-layered replicated commit */
  char *final_revision; /* Computed by layering; if NULL, only using base_revision */
};

enum {
  PROP_0,

  PROP_SYSROOT,
  PROP_OSNAME,
  PROP_FLAGS
};

static void rpmostree_sysroot_upgrader_initable_iface_init (GInitableIface *iface);

G_DEFINE_TYPE_WITH_CODE (RpmOstreeSysrootUpgrader, rpmostree_sysroot_upgrader, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, rpmostree_sysroot_upgrader_initable_iface_init))

static gboolean
parse_origin_deployment (RpmOstreeSysrootUpgrader *self,
                         OstreeDeployment         *deployment,
                         GCancellable           *cancellable,
                         GError                **error)
{
  self->origin = rpmostree_origin_parse_deployment (deployment, error);
  if (!self->origin)
    return FALSE;

  if (rpmostree_origin_get_unconfigured_state (self->origin) &&
      !(self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED))
    {
      /* explicit action is required OS creator to upgrade, print their text as an error */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "origin unconfigured-state: %s",
                   rpmostree_origin_get_unconfigured_state (self->origin));
      return FALSE;
    }

  /* A bit hacky; here we clean out the live state which is deployment specific.
   * We don't expect users of the upgrader to want the live state.
   */
  rpmostree_origin_set_live_state (self->origin, NULL, NULL);

  return TRUE;
}

static gboolean
rpmostree_sysroot_upgrader_initable_init (GInitable        *initable,
                                          GCancellable     *cancellable,
                                          GError          **error)
{
  RpmOstreeSysrootUpgrader *self = (RpmOstreeSysrootUpgrader*)initable;

  OstreeDeployment *booted_deployment =
    ostree_sysroot_get_booted_deployment (self->sysroot);
  if (booted_deployment == NULL && self->osname == NULL)
    return glnx_throw (error, "Not currently booted into an OSTree system and no OS specified");

  if (self->osname == NULL)
    {
      g_assert (booted_deployment);
      self->osname = g_strdup (ostree_deployment_get_osname (booted_deployment));
    }
  else if (self->osname[0] == '\0')
    return glnx_throw (error, "Invalid empty osname");

  if (!ostree_sysroot_get_repo (self->sysroot, &self->repo, cancellable, error))
    return FALSE;

  self->cfg_merge_deployment =
    ostree_sysroot_get_merge_deployment (self->sysroot, self->osname);
  self->origin_merge_deployment =
    rpmostree_syscore_get_origin_merge_deployment (self->sysroot, self->osname);
  if (self->cfg_merge_deployment == NULL || self->origin_merge_deployment == NULL)
    return glnx_throw (error, "No previous deployment for OS '%s'",
                       self->osname);

  /* Should we consider requiring --discard-hotfix here?
   * See also `ostree admin upgrade` bits.
   */
  if (!parse_origin_deployment (self, self->origin_merge_deployment,
                                cancellable, error))
    return FALSE;

  const char *merge_deployment_csum =
    ostree_deployment_get_csum (self->origin_merge_deployment);

  /* Now, load starting base/final checksums.  We may end up changing
   * one or both if we upgrade.  But we also want to support redeploying
   * without changing them.
   */
  gboolean is_layered;
  if (!rpmostree_deployment_get_layered_info (self->repo,
                                              self->origin_merge_deployment,
                                              &is_layered, &self->base_revision,
                                              NULL, NULL, error))
    return FALSE;

  if (is_layered)
    /* base layer is already populated above */
    self->final_revision = g_strdup (merge_deployment_csum);
  else
    self->base_revision = g_strdup (merge_deployment_csum);

  g_assert (self->base_revision);
  return TRUE;
}

static void
rpmostree_sysroot_upgrader_initable_iface_init (GInitableIface *iface)
{
  iface->init = rpmostree_sysroot_upgrader_initable_init;
}

static void
rpmostree_sysroot_upgrader_finalize (GObject *object)
{
  RpmOstreeSysrootUpgrader *self = RPMOSTREE_SYSROOT_UPGRADER (object);

  g_clear_pointer (&self->rsack, rpmostree_refsack_unref);

  if (self->tmprootfs_dfd != -1)
    (void)close (self->tmprootfs_dfd);

  g_clear_pointer (&self->devino_cache, (GDestroyNotify)ostree_repo_devino_cache_unref);

  g_clear_object (&self->sysroot);
  g_clear_object (&self->repo);
  g_free (self->osname);

  g_clear_object (&self->cfg_merge_deployment);
  g_clear_object (&self->origin_merge_deployment);
  g_clear_pointer (&self->origin, (GDestroyNotify)rpmostree_origin_unref);
  g_free (self->base_revision);
  g_free (self->final_revision);

  g_clear_pointer (&self->overlay_packages, (GDestroyNotify)g_ptr_array_unref);

  G_OBJECT_CLASS (rpmostree_sysroot_upgrader_parent_class)->finalize (object);
}

static void
rpmostree_sysroot_upgrader_set_property (GObject         *object,
                                         guint            prop_id,
                                         const GValue    *value,
                                         GParamSpec      *pspec)
{
  RpmOstreeSysrootUpgrader *self = RPMOSTREE_SYSROOT_UPGRADER (object);

  switch (prop_id)
    {
    case PROP_SYSROOT:
      self->sysroot = g_value_dup_object (value);
      break;
    case PROP_OSNAME:
      self->osname = g_value_dup_string (value);
      break;
    case PROP_FLAGS:
      self->flags = g_value_get_flags (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
rpmostree_sysroot_upgrader_get_property (GObject         *object,
                                         guint            prop_id,
                                         GValue          *value,
                                         GParamSpec      *pspec)
{
  RpmOstreeSysrootUpgrader *self = RPMOSTREE_SYSROOT_UPGRADER (object);

  switch (prop_id)
    {
    case PROP_SYSROOT:
      g_value_set_object (value, self->sysroot);
      break;
    case PROP_OSNAME:
      g_value_set_string (value, self->osname);
      break;
    case PROP_FLAGS:
      g_value_set_flags (value, self->flags);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
rpmostree_sysroot_upgrader_constructed (GObject *object)
{
  RpmOstreeSysrootUpgrader *self = RPMOSTREE_SYSROOT_UPGRADER (object);

  g_assert (self->sysroot != NULL);

  G_OBJECT_CLASS (rpmostree_sysroot_upgrader_parent_class)->constructed (object);
}

static void
rpmostree_sysroot_upgrader_class_init (RpmOstreeSysrootUpgraderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = rpmostree_sysroot_upgrader_constructed;
  object_class->get_property = rpmostree_sysroot_upgrader_get_property;
  object_class->set_property = rpmostree_sysroot_upgrader_set_property;
  object_class->finalize = rpmostree_sysroot_upgrader_finalize;

  g_object_class_install_property (object_class,
                                   PROP_SYSROOT,
                                   g_param_spec_object ("sysroot", "", "",
                                                        OSTREE_TYPE_SYSROOT,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class,
                                   PROP_OSNAME,
                                   g_param_spec_string ("osname", "", "", NULL,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class,
                                   PROP_FLAGS,
                                   g_param_spec_flags ("flags", "", "",
                                                       rpmostree_sysroot_upgrader_flags_get_type (),
                                                       0,
                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
rpmostree_sysroot_upgrader_init (RpmOstreeSysrootUpgrader *self)
{
  self->tmprootfs_dfd = -1;
}


/**
 * rpmostree_sysroot_upgrader_new_for_os_with_flags:
 * @sysroot: An #OstreeSysroot
 * @osname: (allow-none): Operating system name
 * @flags: Flags
 *
 * Returns: (transfer full): An upgrader
 */
RpmOstreeSysrootUpgrader *
rpmostree_sysroot_upgrader_new (OstreeSysroot              *sysroot,
                                const char                 *osname,
                                RpmOstreeSysrootUpgraderFlags  flags,
                                GCancellable               *cancellable,
                                GError                    **error)
{
  return g_initable_new (RPMOSTREE_TYPE_SYSROOT_UPGRADER, cancellable, error,
                         "sysroot", sysroot, "osname", osname, "flags", flags, NULL);
}

RpmOstreeOrigin *
rpmostree_sysroot_upgrader_dup_origin (RpmOstreeSysrootUpgrader *self)
{
  return rpmostree_origin_dup (self->origin);
}

void
rpmostree_sysroot_upgrader_set_origin (RpmOstreeSysrootUpgrader *self,
                                       RpmOstreeOrigin          *new_origin)
{
  rpmostree_origin_unref (self->origin);
  self->origin = rpmostree_origin_dup (new_origin);
}

const char *
rpmostree_sysroot_upgrader_get_base (RpmOstreeSysrootUpgrader *self)
{
  return self->base_revision;
}

OstreeDeployment*
rpmostree_sysroot_upgrader_get_merge_deployment (RpmOstreeSysrootUpgrader *self)
{
  return self->origin_merge_deployment;
}

/*
 * Like ostree_sysroot_upgrader_pull(), but modified to handle layered packages.
 */
gboolean
rpmostree_sysroot_upgrader_pull (RpmOstreeSysrootUpgrader  *self,
                                 const char             *dir_to_pull,
                                 OstreeRepoPullFlags     flags,
                                 OstreeAsyncProgress    *progress,
                                 gboolean               *out_changed,
                                 GCancellable           *cancellable,
                                 GError                **error)
{
  g_autofree char *origin_remote = NULL;
  g_autofree char *origin_ref = NULL;
  if (!ostree_parse_refspec (rpmostree_origin_get_refspec (self->origin),
                             &origin_remote, &origin_ref, error))
    return FALSE;

  char *refs_to_fetch[] = { NULL, NULL };
  if (rpmostree_origin_get_override_commit (self->origin) != NULL)
    refs_to_fetch[0] = (char*)rpmostree_origin_get_override_commit (self->origin);
  else
    refs_to_fetch[0] = origin_ref;

  g_assert (self->origin_merge_deployment);
  if (origin_remote)
    {
      if (!ostree_repo_pull_one_dir (self->repo, origin_remote, dir_to_pull, refs_to_fetch,
                                     flags, progress,
                                     cancellable, error))
        return FALSE;

      if (progress)
        ostree_async_progress_finish (progress);
    }

  g_autofree char *new_base_revision = NULL;
  if (rpmostree_origin_get_override_commit (self->origin) != NULL)
    {
      if (!ostree_repo_set_ref_immediate (self->repo,
                                          origin_remote,
                                          origin_ref,
                                          rpmostree_origin_get_override_commit (self->origin),
                                          cancellable,
                                          error))
        return FALSE;

      new_base_revision = g_strdup (rpmostree_origin_get_override_commit (self->origin));
    }
  else
    {
      if (!ostree_repo_resolve_rev (self->repo, rpmostree_origin_get_refspec (self->origin), FALSE,
                                    &new_base_revision, error))
        return FALSE;
    }

  {
    gboolean allow_older =
      (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER) > 0;
    gboolean changed = FALSE;

    if (strcmp (new_base_revision, self->base_revision) != 0)
      {
        changed = TRUE;

        if (!allow_older)
          {
            if (!ostree_sysroot_upgrader_check_timestamps (self->repo, self->base_revision,
                                                           new_base_revision,
                                                           error))
              return FALSE;
          }

        g_free (self->base_revision);
        self->base_revision = g_steal_pointer (&new_base_revision);
      }

    *out_changed = changed;
  }

  return TRUE;
}

static gboolean
checkout_base_tree (RpmOstreeSysrootUpgrader *self,
                    GCancellable          *cancellable,
                    GError               **error)
{
  g_assert_cmpint (self->tmprootfs_dfd, ==, -1);

  /* let's give the user some feedback so they don't think we're blocked */
  rpmostree_output_task_begin ("Checking out tree %.7s", self->base_revision);

  int repo_dfd = ostree_repo_get_dfd (self->repo); /* borrowed */
  if (!glnx_shutil_mkdir_p_at (repo_dfd,
                               dirname (strdupa (RPMOSTREE_TMP_ROOTFS_DIR)),
                               0755, cancellable, error))
    return FALSE;

  /* delete dir in case a previous run didn't finish successfully */
  if (!glnx_shutil_rm_rf_at (repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR,
                             cancellable, error))
    return FALSE;

  /* NB: we let ostree create the dir for us so that the root dir has the
   * correct xattrs (e.g. selinux label) */
  self->devino_cache = ostree_repo_devino_cache_new ();
  OstreeRepoCheckoutAtOptions checkout_options =
    { .devino_to_csum_cache = self->devino_cache };
  if (!ostree_repo_checkout_at (self->repo, &checkout_options,
                                repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR,
                                self->base_revision, cancellable, error))
    return FALSE;

  if (!glnx_opendirat (repo_dfd, RPMOSTREE_TMP_ROOTFS_DIR, FALSE,
                       &self->tmprootfs_dfd, error))
    return FALSE;

  /* build a centralized rsack for it, since we need it in a few places */
  self->rsack = rpmostree_get_refsack_for_root (self->tmprootfs_dfd, ".",
                                                cancellable, error);
  if (self->rsack == NULL)
    return FALSE;

  rpmostree_output_task_end ("done");

  return TRUE;
}

/* XXX: This is ugly, but the alternative is to de-couple RpmOstreeTreespec from
 * RpmOstreeContext, which also use it for hashing and store it directly in
 * assembled commit metadata. Probably assemble_commit() should live somewhere
 * else, maybe directly in `container-builtins.c`. */
static RpmOstreeTreespec *
generate_treespec (RpmOstreeSysrootUpgrader *self)
{
  g_autoptr(GKeyFile) treespec = g_key_file_new ();

  GPtrArray *packages = self->overlay_packages;
  if (packages->len > 0)
    {
      g_key_file_set_string_list (treespec, "tree", "packages",
                                  (const char* const*)packages->pdata,
                                  packages->len);
    }

  GHashTable *local_packages =
    rpmostree_origin_get_local_packages (self->origin);
  if (g_hash_table_size (local_packages) > 0)
    {
      g_autoptr(GPtrArray) sha256_nevra =
        g_ptr_array_new_with_free_func (g_free);

      gpointer k, v;
      GHashTableIter it;
      g_hash_table_iter_init (&it, local_packages);
      while (g_hash_table_iter_next (&it, &k, &v))
        g_ptr_array_add (sha256_nevra, g_strconcat (v, ":", k, NULL));

      g_key_file_set_string_list (treespec, "tree", "cached-packages",
                                  (const char* const*)sha256_nevra->pdata,
                                  sha256_nevra->len);
    }

  GHashTable *overrides_remove =
    rpmostree_origin_get_overrides_remove (self->origin);
  if (g_hash_table_size (overrides_remove) > 0)
    {
      g_autofree char **pkgv =
        (char**) g_hash_table_get_keys_as_array (overrides_remove, NULL);
      g_key_file_set_string_list (treespec, "tree",
                                  "removed-base-packages",
                                  (const char* const*)pkgv,
                                  g_strv_length (pkgv));
    }

  return rpmostree_treespec_new_from_keyfile (treespec, NULL);
}

static gboolean
finalize_overrides (RpmOstreeSysrootUpgrader *self,
                    GCancellable             *cancellable,
                    GError                  **error)
{
  /* Removal overrides have the magical property that they drop out if the base layer no
   * longer has them. This keeps the origin consistent. New removal overrides are checked in
   * deploy_transaction_execute() to ensure they're valid; i.e. the pkgs exist. */

  GHashTable *removals = rpmostree_origin_get_overrides_remove (self->origin);

  if (g_hash_table_size (removals) == 0)
    return TRUE;

  /* NB: strings are owned by hash table */
  g_autoptr(GPtrArray) removals_to_remove = g_ptr_array_new ();

  GHashTableIter it;
  gpointer itkey;
  g_hash_table_iter_init (&it, removals);
  while (g_hash_table_iter_next (&it, &itkey, NULL))
    {
      /* only match pkgname */
      const char *pkgname = itkey;
      hy_autoquery HyQuery query = hy_query_create (self->rsack->sack);
      hy_query_filter (query, HY_PKG_NAME, HY_EQ, pkgname);
      g_autoptr(GPtrArray) pkgs = hy_query_run (query);

      if (pkgs->len == 0)
        g_ptr_array_add (removals_to_remove, (gpointer)pkgname);
    }

  if (removals_to_remove->len > 0)
    {
      g_ptr_array_add (removals_to_remove, NULL);
      if (!rpmostree_origin_remove_overrides (self->origin,
                                              (char**)removals_to_remove->pdata, error))
        return FALSE;
    }

  return TRUE;
}

/* Go through rpmdb and jot down the missing pkgs from the given set. Really, we
 * don't *have* to do this: we could just give everything to libdnf and let it
 * figure out what is already installed. The advantage of doing it ourselves is
 * that we can optimize for the case where no packages are missing and we don't
 * have to fetch metadata. Another advantage is that we can make sure that the
 * embedded treespec only contains the provides we *actually* layer, which is
 * needed for displaying to the user (though we could fetch that info from
 * libdnf after resolution).
 * */
static gboolean
finalize_packages_to_overlay (RpmOstreeSysrootUpgrader *self,
                              GCancellable             *cancellable,
                              GError                  **error)
{
  gboolean ret = FALSE;
  g_autofree char *metadata_tmp_path = NULL;
  g_autoptr(GPtrArray) ret_missing_pkgs =
    g_ptr_array_new_with_free_func (g_free);

  GHashTableIter it;
  gpointer itkey, itval;

  /* Add the local pkgs as if they were installed: since they're unconditionally
   * layered, we treat them as part of the base wrt regular requested pkgs. E.g.
   * you can have foo-1.0-1.x86_64 layered, and foo or /usr/bin/foo as dormant.
   * */
  GHashTable *local_pkgs = rpmostree_origin_get_local_packages (self->origin);
  if (g_hash_table_size (local_pkgs) > 0)
    {
      glnx_unref_object OstreeRepo *pkgcache_repo = NULL;

      if (!rpmostree_mkdtemp ("/tmp/rpmostree-localpkgmeta-XXXXXX",
                              &metadata_tmp_path, NULL, error))
        goto out;

      if (!rpmostree_get_pkgcache_repo (self->repo, &pkgcache_repo,
                                        cancellable, error))
        goto out;

      g_hash_table_iter_init (&it, local_pkgs);
      while (g_hash_table_iter_next (&it, &itkey, &itval))
        {
          const char *nevra = itkey;
          g_autoptr(GVariant) header = NULL;
          g_autofree char *path =
            g_strdup_printf ("%s/%s.rpm", metadata_tmp_path, nevra);

          if (!rpmostree_pkgcache_find_pkg_header (pkgcache_repo, nevra, itval,
                                                   &header, cancellable, error))
            goto out;

          if (!glnx_file_replace_contents_at (AT_FDCWD, path,
                                              g_variant_get_data (header),
                                              g_variant_get_size (header),
                                              GLNX_FILE_REPLACE_NODATASYNC,
                                              cancellable, error))
            goto out;

          /* Also check if that exact NEVRA is already in the root (if the pkg
           * exists, but is a different EVR, depsolve will catch that). In the
           * future, we'll allow packages to replace base pkgs. */
          if (rpmostree_sack_has_subject (self->rsack->sack, nevra))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Package '%s' is already in the base", nevra);
              goto out;
            }

          dnf_sack_add_cmdline_package (self->rsack->sack, path);
        }
    }

  GHashTable *removals = rpmostree_origin_get_overrides_remove (self->origin);

  /* check for each package if we have a provides or a path match */
  g_hash_table_iter_init (&it, rpmostree_origin_get_packages (self->origin));
  while (g_hash_table_iter_next (&it, &itkey, NULL))
    {
      const char *pattern = itkey;
      g_autoptr(GPtrArray) matches =
        rpmostree_get_matching_packages (self->rsack->sack, pattern);

      if (matches->len == 0)
        {
          /* no matches, so we'll need to layer it */
          g_ptr_array_add (ret_missing_pkgs, g_strdup (itkey));
          continue;
        }

      /* Error out if it matches a base package that was also requested to be removed.
       * Conceptually, we want users to use override replacements, not remove+overlay. */
      for (guint i = 0; i < matches->len; i++)
        {
          DnfPackage *pkg = matches->pdata[i];
          const char *name = dnf_package_get_name (pkg);
          const char *repo = dnf_package_get_reponame (pkg);

          if (g_strcmp0 (repo, HY_CMDLINE_REPO_NAME) == 0)
            continue; /* local RPM added up above */

          if (g_hash_table_contains (removals, name))
            {
              glnx_throw (error, "Cannot request '%s' provided by removed package '%s'",
                          pattern, dnf_package_get_nevra (pkg));
            }
        }

      /* otherwise, it's a dormant package */
    }

  self->overlay_packages = g_steal_pointer (&ret_missing_pkgs);
  ret = TRUE;
out:
  if (metadata_tmp_path)
    (void) glnx_shutil_rm_rf_at (AT_FDCWD, metadata_tmp_path, NULL, NULL);
  return ret;
}

static gboolean
prepare_context_for_assembly (RpmOstreeSysrootUpgrader *self,
                              RpmOstreeContext         *ctx,
                              const char               *tmprootfs,
                              GCancellable             *cancellable,
                              GError                  **error)
{
  DnfContext *hifctx = rpmostree_context_get_hif (ctx);

  const char *sysroot_path =
    gs_file_get_path_cached (ostree_sysroot_get_path (self->sysroot));
  g_autofree char *merge_deployment_dirpath =
    ostree_sysroot_get_deployment_dirpath (self->sysroot,
                                           self->cfg_merge_deployment);
  g_autofree char *merge_deployment_root =
    g_build_filename (sysroot_path, merge_deployment_dirpath, NULL);

  g_autofree char *reposdir = g_build_filename (merge_deployment_root,
                                                "etc/yum.repos.d", NULL);
  g_autofree char *passwddir = g_build_filename (merge_deployment_root,
                                                 "etc", NULL);

  /* point libhif to the yum.repos.d and os-release of the merge deployment */
  dnf_context_set_repo_dir (hifctx, reposdir);
  dnf_context_set_source_root (hifctx, tmprootfs);

  /* point the core to the passwd & group of the merge deployment */
  rpmostree_context_set_passwd_dir (ctx, passwddir);

  /* load the sepolicy to use during import */
  glnx_unref_object OstreeSePolicy *sepolicy = NULL;
  if (!rpmostree_prepare_rootfs_get_sepolicy (self->tmprootfs_dfd, &sepolicy,
                                              cancellable, error))
    return FALSE;

  rpmostree_context_set_sepolicy (ctx, sepolicy);
  return TRUE;
}

static gboolean
do_local_assembly (RpmOstreeSysrootUpgrader *self,
                   GCancellable             *cancellable,
                   GError                  **error)
{
  g_autoptr(RpmOstreeContext) ctx =
    rpmostree_context_new_system (cancellable, error);

  g_autofree char *tmprootfs_abspath =
    glnx_fdrel_abspath (self->tmprootfs_dfd, ".");

  if (!prepare_context_for_assembly (self, ctx, tmprootfs_abspath,
                                     cancellable, error))
    return FALSE;

  GHashTable *local_pkgs = rpmostree_origin_get_local_packages (self->origin);

  /* NB: We're pretty much using the defaults for the other treespec values like
   * instlang and docs since it would be hard to expose the cli for them because
   * they wouldn't affect just the new pkgs, but even previously added ones. */
  g_autoptr(RpmOstreeTreespec) treespec = generate_treespec (self);
  if (treespec == NULL)
    return FALSE;

  if (!rpmostree_context_setup (ctx, tmprootfs_abspath, NULL,
                                treespec, cancellable, error))
    return FALSE;

  g_autoptr(OstreeRepo) pkgcache_repo = NULL;
  if (!rpmostree_get_pkgcache_repo (self->repo, &pkgcache_repo,
                                    cancellable, error))
    return FALSE;

  rpmostree_context_set_repos (ctx, self->repo, pkgcache_repo);

  GHashTable *overrides_remove =
    rpmostree_origin_get_overrides_remove (self->origin);
  const gboolean have_packages = (self->overlay_packages->len > 0 ||
                                  g_hash_table_size (local_pkgs) > 0 ||
                                  g_hash_table_size (overrides_remove) > 0);

  if (have_packages)
    {
      if (!rpmostree_context_prepare (ctx, cancellable, error))
        return FALSE;
    }
  else
    rpmostree_context_set_is_empty (ctx);

  if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN)
    {
      if (have_packages)
        rpmostree_print_transaction (rpmostree_context_get_hif (ctx));
      return TRUE; /* Note early return */
    }

  if (have_packages)
    {
      if (!rpmostree_context_download (ctx, cancellable, error))
        return FALSE;
      if (!rpmostree_context_import (ctx, cancellable, error))
        return FALSE;
      if (!rpmostree_context_relabel (ctx, cancellable, error))
        return FALSE;

      g_clear_pointer (&self->final_revision, g_free);
      gboolean noscripts =
        (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_PKGOVERLAY_NOSCRIPTS) > 0;

      /* --- override/overlay and commit --- */
      if (!rpmostree_context_assemble_tmprootfs (ctx, self->tmprootfs_dfd,
                                                 self->devino_cache, noscripts,
                                                 cancellable, error))
        return FALSE;
    }

  if (!rpmostree_rootfs_postprocess_common (self->tmprootfs_dfd, cancellable, error))
    return FALSE;

  if (rpmostree_origin_get_regenerate_initramfs (self->origin))
    {
      g_auto(GLnxTmpfile) initramfs_tmpf = { 0, };
      const char *bootdir;
      const char *kver;
      const char *kernel_path;
      const char *initramfs_path;
      g_autoptr(GVariant) kernel_state = NULL;
      const char *const* add_dracut_argv = NULL;

      add_dracut_argv = rpmostree_origin_get_initramfs_args (self->origin);

      rpmostree_output_task_begin ("Generating initramfs");

      kernel_state = rpmostree_find_kernel (self->tmprootfs_dfd, cancellable, error);
      if (!kernel_state)
        return FALSE;
      g_variant_get (kernel_state, "(&s&s&sm&s)",
                     &kver, &bootdir,
                     &kernel_path, &initramfs_path);
      g_assert (initramfs_path);

      if (!rpmostree_run_dracut (self->tmprootfs_dfd, add_dracut_argv, kver,
                                 initramfs_path, &initramfs_tmpf,
                                 cancellable, error))
        return FALSE;

      if (!rpmostree_finalize_kernel (self->tmprootfs_dfd, bootdir, kver, kernel_path,
                                      &initramfs_tmpf,
                                      cancellable, error))
        return FALSE;

      rpmostree_output_task_end ("done");
    }

  if (!rpmostree_context_commit_tmprootfs (ctx, self->tmprootfs_dfd, self->devino_cache,
                                           self->base_revision,
                                           RPMOSTREE_ASSEMBLE_TYPE_CLIENT_LAYERING,
                                           &self->final_revision,
                                           cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
requires_local_assembly (RpmOstreeSysrootUpgrader *self)
{
  GHashTable *local_pkgs = rpmostree_origin_get_local_packages (self->origin);
  GHashTable *overrides_remove =
    rpmostree_origin_get_overrides_remove (self->origin);

  /* Now, it's possible all requested packages are in the new tree, so we have
   * another optimization here for that case. This is a bit tricky: assuming we
   * came here from an 'rpm-ostree install', this might mean that we redeploy
   * the exact same base layer, with the only difference being the origin file.
   * We could down the line experiment with optimizing this by just updating the
   * merge deployment's origin.
   * https://github.com/projectatomic/rpm-ostree/issues/753
   */

  return g_hash_table_size (local_pkgs) > 0 ||
         g_hash_table_size (overrides_remove) > 0 ||
         self->overlay_packages->len > 0 ||
         rpmostree_origin_get_regenerate_initramfs (self->origin);
}

/* Determines whether local assembly is required and does it if so. */
static gboolean
maybe_do_local_assembly (RpmOstreeSysrootUpgrader *self,
                         GCancellable             *cancellable,
                         GError                  **error)
{
  if (!checkout_base_tree (self, cancellable, error))
    return FALSE;

  if (!finalize_overrides (self, cancellable, error))
    return FALSE;

  if (!finalize_packages_to_overlay (self, cancellable, error))
    return FALSE;

  if (!requires_local_assembly (self))
    {
      /* no assembly required; clear final_revision to ensure we deploy base */
      g_clear_pointer (&self->final_revision, g_free);
      return TRUE;
    }

  return do_local_assembly (self, cancellable, error);
}

/**
 * rpmostree_sysroot_upgrader_deploy:
 * @self: Self
 * @cancellable: Cancellable
 * @error: Error
 *
 * Write the new deployment to disk, overlay any packages requested, perform a
 * configuration merge with /etc, and update the bootloader configuration.
 */
gboolean
rpmostree_sysroot_upgrader_deploy (RpmOstreeSysrootUpgrader *self,
                                   GCancellable             *cancellable,
                                   GError                  **error)
{
  glnx_unref_object OstreeDeployment *new_deployment = NULL;
  const char *target_revision;
  g_autoptr(GKeyFile) origin = NULL;

  if (rpmostree_origin_may_require_local_assembly (self->origin))
    {
      if (!maybe_do_local_assembly (self, cancellable, error))
        return FALSE;
    }
  else
    g_clear_pointer (&self->final_revision, g_free);

  if (self->flags & RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN)
    {
      /* we already printed the transaction in do_final_local_assembly() */
      return TRUE;
    }

  /* make sure we have a known target to deploy */
  target_revision = self->final_revision ?: self->base_revision;
  g_assert (target_revision);

  origin = rpmostree_origin_dup_keyfile (self->origin);
  if (!ostree_sysroot_deploy_tree (self->sysroot, self->osname,
                                   target_revision, origin,
                                   self->cfg_merge_deployment,
                                   NULL,
                                   &new_deployment,
                                   cancellable, error))
    return FALSE;

  if (self->final_revision)
    {
      /* Generate a temporary ref for the new deployment in case we are
       * interrupted; the base layer refs generation isn't transactional.
       */
      if (!ostree_repo_set_ref_immediate (self->repo, NULL, RPMOSTREE_TMP_BASE_REF,
                                          self->base_revision,
                                          cancellable, error))
        return FALSE;
    }

  g_autoptr(GPtrArray) new_deployments =
    rpmostree_syscore_add_deployment (self->sysroot, new_deployment,
                                      self->cfg_merge_deployment, FALSE, error);
  if (!new_deployments)
    return FALSE;
  if (!rpmostree_syscore_write_deployments (self->sysroot, self->repo, new_deployments,
                                            cancellable, error))
    return FALSE;

  return TRUE;
}

GType
rpmostree_sysroot_upgrader_flags_get_type (void)
{
  static volatile gsize g_define_type_id__volatile = 0;

  if (g_once_init_enter (&g_define_type_id__volatile))
    {
      static const GFlagsValue values[] = {
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED,
          "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_IGNORE_UNCONFIGURED",
          "ignore-unconfigured" },
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER,
          "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_ALLOW_OLDER",
          "allow-older" },
        { RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN,
          "RPMOSTREE_SYSROOT_UPGRADER_FLAGS_DRY_RUN",
          "dry-run" }
      };
      GType g_define_type_id =
        g_flags_register_static (g_intern_static_string ("RpmOstreeSysrootUpgraderFlags"), values);
      g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

  return g_define_type_id__volatile;
}
