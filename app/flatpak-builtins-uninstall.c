/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <glib/gi18n.h>

#include "libglnx/libglnx.h"

#include "flatpak-builtins.h"
#include "flatpak-builtins-utils.h"
#include "flatpak-utils.h"
#include "flatpak-error.h"

static char *opt_arch;
static gboolean opt_keep_ref;
static gboolean opt_force_remove;
static gboolean opt_no_related;
static gboolean opt_runtime;
static gboolean opt_app;

static GOptionEntry options[] = {
  { "arch", 0, 0, G_OPTION_ARG_STRING, &opt_arch, N_("Arch to uninstall"), N_("ARCH") },
  { "keep-ref", 0, 0, G_OPTION_ARG_NONE, &opt_keep_ref, N_("Keep ref in local repository"), NULL },
  { "no-related", 0, 0, G_OPTION_ARG_NONE, &opt_no_related, N_("Don't uninstall related refs"), NULL },
  { "force-remove", 0, 0, G_OPTION_ARG_NONE, &opt_force_remove, N_("Remove files even if running"), NULL },
  { "runtime", 0, 0, G_OPTION_ARG_NONE, &opt_runtime, N_("Look for runtime with the specified name"), NULL },
  { "app", 0, 0, G_OPTION_ARG_NONE, &opt_app, N_("Look for app with the specified name"), NULL },
  { NULL }
};

typedef struct {
  FlatpakDir *dir;
  GHashTable *refs_hash;
  GPtrArray *refs;
} UninstallDir;

static UninstallDir *
uninstall_dir_new (FlatpakDir *dir)
{
  UninstallDir *udir = g_new0 (UninstallDir, 1);

  udir->dir = g_object_ref (dir);
  udir->refs = g_ptr_array_new_with_free_func (g_free);
  udir->refs_hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  return udir;
}

static void
uninstall_dir_free (UninstallDir *udir)
{
  g_object_unref (udir->dir);
  g_hash_table_unref (udir->refs_hash);
  g_ptr_array_unref (udir->refs);
  g_free (udir);
}

static void
uninstall_dir_add_ref (UninstallDir *udir,
                       const char *ref)
{
  if (g_hash_table_insert (udir->refs_hash, g_strdup (ref), NULL))
    g_ptr_array_add (udir->refs, g_strdup (ref));
}

static UninstallDir *
uninstall_dir_ensure (GHashTable *uninstall_dirs,
                      FlatpakDir *dir)
{
  UninstallDir *udir;

  udir = g_hash_table_lookup (uninstall_dirs, dir);
  if (udir == NULL)
    {
      udir = uninstall_dir_new (dir);
      g_hash_table_insert (uninstall_dirs, udir->dir, udir);
    }

  return udir;
}


gboolean
flatpak_builtin_uninstall (int argc, char **argv, GCancellable *cancellable, GError **error)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  char **prefs = NULL;
  int i, j, k, n_prefs;
  const char *default_branch = NULL;
  FlatpakHelperUninstallFlags flags = 0;
  g_autoptr(GPtrArray) related = NULL;
  FlatpakKinds kinds;
  FlatpakKinds kind;
  g_autoptr(GHashTable) uninstall_dirs = NULL;

  context = g_option_context_new (_("REF... - Uninstall an application"));
  g_option_context_set_translation_domain (context, GETTEXT_PACKAGE);

  if (!flatpak_option_context_parse (context, options, &argc, &argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS,
                                     &dirs, cancellable, error))
    return FALSE;

  if (argc < 2)
    return usage_error (context, _("Must specify at least one REF"), error);

  prefs = &argv[1];
  n_prefs = argc - 1;

  /* Backwards compat for old "REPOSITORY NAME [BRANCH]" argument version */
  if (argc == 3 && looks_like_branch (argv[2]))
    {
      default_branch = argv[2];
      n_prefs = 1;
    }

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);
  uninstall_dirs = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)uninstall_dir_free);

  for (j = 0; j < n_prefs; j++)
    {
      const char *pref = NULL;
      FlatpakKinds matched_kinds;
      g_autofree char *id = NULL;
      g_autofree char *arch = NULL;
      g_autofree char *branch = NULL;
      g_autoptr(GError) local_error = NULL;
      g_autoptr(GError) first_error = NULL;
      g_autofree char *first_ref = NULL;
      g_autofree char *origin = NULL;
      g_autoptr(GPtrArray) dirs_with_ref = NULL;
      UninstallDir *udir = NULL;

      pref = prefs[j];

      if (!flatpak_split_partial_ref_arg (pref, kinds, opt_arch, default_branch,
                                          &matched_kinds, &id, &arch, &branch, error))
        return FALSE;

      dirs_with_ref = g_ptr_array_new ();
      for (k = 0; k < dirs->len; k++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, k);
          g_autofree char *ref = NULL;

          ref = flatpak_dir_find_installed_ref (dir, id, branch, arch,
                                                kinds, &kind, &local_error);
          if (ref == NULL)
            {
              if (g_error_matches (local_error, FLATPAK_ERROR, FLATPAK_ERROR_NOT_INSTALLED))
                {
                  if (first_error == NULL)
                    first_error = g_steal_pointer (&local_error);
                  g_clear_error (&local_error);
                }
              else
                {
                  g_propagate_error (error, g_steal_pointer (&local_error));
                  return FALSE;
                }
            }
          else
            {
              g_ptr_array_add (dirs_with_ref, dir);
              if (first_ref == NULL)
                first_ref = g_strdup (ref);
            }
        }

      if (dirs_with_ref->len == 0)
        {
          g_assert (first_error != NULL);
          /* No match anywhere, return the first NOT_INSTALLED error */
          g_propagate_error (error, g_steal_pointer (&first_error));
          return FALSE;
        }

      if (dirs_with_ref->len > 1)
        {
          g_autoptr(GString) dir_names = g_string_new ("");
          for (k = 0; k < dirs_with_ref->len; k++)
            {
              FlatpakDir *dir = g_ptr_array_index (dirs_with_ref, k);
              g_autofree char *dir_name = flatpak_dir_get_name (dir);
              if (k > 0)
                g_string_append (dir_names, ", ");
              g_string_append (dir_names, dir_name);
            }

          return flatpak_fail (error,
                               _("Ref ‘%s’ found in multiple installations: %s. You must specify one."),
                               pref, dir_names->str);
        }

      udir = uninstall_dir_ensure (uninstall_dirs, g_ptr_array_index (dirs_with_ref, 0));

      g_assert (first_ref);

      uninstall_dir_add_ref (udir, first_ref);

      /* TODO: when removing runtimes, look for apps that use it, require --force */

      if (opt_no_related)
        continue;

      origin = flatpak_dir_get_origin (udir->dir, first_ref, NULL, NULL);
      if (origin == NULL)
        continue;

      related = flatpak_dir_find_local_related (udir->dir, first_ref, origin,
                                                NULL, &local_error);
      if (related == NULL)
        {
          g_printerr (_("Warning: Problem looking for related refs: %s\n"),
                      local_error->message);
          continue;
        }

      for (i = 0; i < related->len; i++)
        {
          FlatpakRelated *rel = g_ptr_array_index (related, i);
          g_autoptr(GVariant) deploy_data = NULL;

          if (!rel->delete)
            continue;

          deploy_data = flatpak_dir_get_deploy_data (udir->dir, rel->ref, NULL, NULL);
          if (deploy_data != NULL)
            uninstall_dir_add_ref (udir, rel->ref);
        }
    }

  if (opt_keep_ref)
    flags |= FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF;
  if (opt_force_remove)
    flags |= FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE;

  GLNX_HASH_TABLE_FOREACH_V (uninstall_dirs, UninstallDir *, udir)
    {
      g_autofree char *dir_name = flatpak_dir_get_name (udir->dir);

      for (i = 0; i < udir->refs->len; i++)
        {
          const char *ref = (char *)g_ptr_array_index (udir->refs, i);
          const char *pref = strchr (ref, '/') + 1;

          g_print (_("Uninstalling: %s from %s\n"), pref, dir_name);

          if (!flatpak_dir_uninstall (udir->dir, ref, flags,
                                      cancellable, error))
            return FALSE;
        }
    }

  return TRUE;
}

gboolean
flatpak_complete_uninstall (FlatpakCompletion *completion)
{
  g_autoptr(GOptionContext) context = NULL;
  g_autoptr(GPtrArray) dirs = NULL;
  int i;
  FlatpakKinds kinds;

  context = g_option_context_new ("");
  if (!flatpak_option_context_parse (context, options, &completion->argc, &completion->argv,
                                     FLATPAK_BUILTIN_FLAG_STANDARD_DIRS, &dirs, NULL, NULL))
    return FALSE;

  kinds = flatpak_kinds_from_bools (opt_app, opt_runtime);

  switch (completion->argc)
    {
    case 0:
    default: /* REF */
      flatpak_complete_options (completion, global_entries);
      flatpak_complete_options (completion, options);
      flatpak_complete_options (completion, user_entries);

      for (i = 0; i < dirs->len; i++)
        {
          FlatpakDir *dir = g_ptr_array_index (dirs, i);
          flatpak_complete_partial_ref (completion, kinds, opt_arch, dir, NULL);
        }

      break;
    }

  return TRUE;
}
