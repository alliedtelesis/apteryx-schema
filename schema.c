/**
 * @file schema.c
 * Database schema support for Apteryx.
 *
 * Copyright 2019, Allied Telesis Labs New Zealand, Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>
 */
#include "internal.h"
#include <dirent.h>
#include <fnmatch.h>
#include "apteryx-schema.h"

/* Debug */
bool apteryx_schema_debug = false;

struct apteryx_schema_model *
model_create (char *name, char *organization, char *version)
{
    struct apteryx_schema_model *model;
    model = calloc (1, sizeof (struct apteryx_schema_model));
    model->name = name;
    model->organization = organization;
    model->version = version;
    return model;
}

void
model_destroy (struct apteryx_schema_model *model)
{
    free (model->name);
    free (model->organization);
    free (model->version);
    free (model);
}

struct apteryx_schema_node *
node_create (const char *name)
{
    struct apteryx_schema_node *node;
    node = calloc (1, sizeof (struct apteryx_schema_node));
    g_strlcpy (node->name, name, 256);
    return node;
}

void
node_destroy (struct apteryx_schema_node *node)
{
    g_list_free_full (node->children, (GDestroyNotify) node_destroy);
    free (node->description);
    free (node->defvalue);
    free (node->value);
    free (node->pattern);
    free (node);
}

static void
list_schema_files (GList **files, const char *path)
{
    DIR *dp;
    struct dirent *ep;
    char *saveptr = NULL;
    char *cpath;
    char *dpath;

    cpath = strdup (path);
    dpath = strtok_r (cpath, ":", &saveptr);
    while (dpath != NULL)
    {
        dp = opendir (dpath);
        if (dp != NULL)
        {
            while ((ep = readdir (dp)))
            {
                char *filename = NULL;
                if (true
#ifdef HAVE_LIBXML
                    && fnmatch ("*.xml", ep->d_name, 0) != 0
                    && fnmatch ("*.xml.gz", ep->d_name, 0) != 0
#endif
#ifdef HAVE_LIBYANG
                    && fnmatch ("*.yang", ep->d_name, 0) != 0
#endif
                    )
                {
                    continue;
                }
                if (asprintf (&filename, "%s/%s", dpath, ep->d_name) > 0)
                {
                    *files = g_list_append (*files, filename);
                }
            }
            (void) closedir (dp);
        }
        dpath = strtok_r (NULL, ":", &saveptr);
    }
    free (cpath);
    *files = g_list_sort (*files, (GCompareFunc) strcasecmp);
    return;
}

static void
merge_nodes (struct apteryx_schema_node *orig, struct apteryx_schema_node *new, int depth)
{
    apteryx_schema_node *n;
    apteryx_schema_node *o;
    GList *n_iter;
    GList *o_iter;
    GList *stolen = NULL;

    /* For all the new children of this node */
    for (n_iter = new->children; n_iter; n_iter = g_list_next (n_iter))
    {
        n = (struct apteryx_schema_node *) n_iter->data;

        /* Find a match in the original tree */
        for (o_iter = orig->children; o_iter; o_iter = g_list_next (o_iter))
        {
            o = (struct apteryx_schema_node *) o_iter->data;
            if (g_strcmp0 (n->name, o->name) == 0)
            {
                break;
            }
        }
        if (o_iter)
        {
            /* Matching node - merge children */
            merge_nodes (o, n, depth + 1);
        }
        else
        {
            /* Does not match - steal it */
            stolen = g_list_append (stolen, n);
        }
    }

    /* Handle the stolen nodes */
    for (n_iter = stolen; n_iter; n_iter = g_list_next (n_iter))
    {
        new->children = g_list_remove (new->children, n_iter->data);
    }
    orig->children = g_list_concat (orig->children, stolen);

    return;
}

apteryx_schema_instance *
apteryx_schema_load (const char *folders)
{
    struct apteryx_schema_instance *schema;
    GList *files = NULL;
    GList *iter;

    schema = calloc (1, sizeof (struct apteryx_schema_instance));
    if (!schema)
    {
        ERROR ("APTERYX_SCHEMA: Memory allocation error.\n");
        return NULL;
    }
    schema->roots = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) node_destroy);

    /* Load all schema files in the path */
    list_schema_files (&files, folders);
    for (iter = files; iter; iter = g_list_next (iter))
    {
        char *filename = (char *) iter->data;
        apteryx_schema_node *root = NULL;
        char *name = NULL;
        char *organization = NULL;
        char *version = NULL;

        DEBUG ("APTERYX_SCHEMA: Parse %s\n", filename);
#ifdef HAVE_LIBXML
        if (fnmatch ("*.xml", filename, 0) == 0 || fnmatch ("*.xml.gz", filename, 0) == 0)
        {
            root = xml_schema_load (filename);
        }
#endif
#ifdef HAVE_LIBYANG
        if (fnmatch ("*.yang", filename, 0) == 0)
        {
            root = yang_schema_load (filename, &name, &organization, &version);
        }
#endif
        if (root)
        {
            apteryx_schema_node *orig = NULL;

            /* Check if this needs merging in */
            orig = (struct apteryx_schema_node *) g_hash_table_lookup (schema->roots, root->name);
            if (orig)
            {
                /* Merge into the original tree */
                DEBUG ("APTERYX_SCHEMA: Merging \"%s\" into  \"/%s\"\n", filename, orig->name);
                merge_nodes (orig, root, 0);
                node_destroy (root);
            }
            else
            {
                /* Add to the hash table as a new root */
                g_hash_table_replace (schema->roots, g_strdup (root->name), root);
            }

            /* Add to model list */
            if (name)
                schema->models = g_list_prepend (schema->models, model_create (name, organization, version));
        }
        else
        {
            ERROR ("APTERYX-SCHEMA: Failed to parse schema from file \"%s\".\n", filename);
        }
    }
    g_list_free_full (files, free);

    /* Ensure creation order of models */
    schema->models = g_list_reverse (schema->models);
    return schema;
}

void
apteryx_schema_free (apteryx_schema_instance *schema)
{
    g_hash_table_destroy (schema->roots);
    g_list_free_full (schema->models, (GDestroyNotify) model_destroy);
    free (schema);
}

static void
_node_dump (GString *buffer, struct apteryx_schema_node *node, int depth)
{
    GList *iter;

    GString *s = g_string_new (NULL);
    g_string_append_printf (s, "%*s%s", depth * 2, " ", node->name);
    if (node->flags & NODE_FLAGS_ENUM)
    {
        g_string_append_printf (s, "[%s]", node->value);
    }
    else if (node->flags && (node->flags != NODE_FLAGS_LEAF))
    {
        g_string_append_printf (s, "[");
        if (node->flags & NODE_FLAGS_READ)
            g_string_append_printf (s, "r");
        if (node->flags & NODE_FLAGS_WRITE)
            g_string_append_printf (s, "w");
        g_string_append_printf (s, "]");
    }
    if (node->description)
    {
        int pad = (s->len >= 32) ? 0 : (32 - s->len);
        g_string_append_printf (s, "%*s\"%s\"", pad, " ", node->description);
    }
    if (node->defvalue)
    {
        g_string_append_printf (s, " %s", node->defvalue);
    }
    if (node->pattern)
    {
        g_string_append_printf (s, " %s", node->pattern);
    }
    g_string_append_printf (s, "\n");
    g_string_append (buffer, g_string_free (s, false));

    for (iter = node->children; iter; iter = g_list_next (iter))
    {
        _node_dump (buffer, (struct apteryx_schema_node *) iter->data, depth + 1);
    }
}

static void
_schema_dump (gpointer key, gpointer value, gpointer user_data)
{
    _node_dump ((GString *) user_data, (struct apteryx_schema_node *) value, 0);
}

void
apteryx_schema_dump (FILE *fp, apteryx_schema_instance *schema)
{
    GString *s = g_string_new (NULL);
    g_hash_table_foreach (schema->roots, _schema_dump, (gpointer) s);
    fprintf (fp, "%s", g_string_free (s, false));
}

apteryx_schema_model*
apteryx_schema_first_model (apteryx_schema_instance *schema)
{
    return schema->models ? g_list_first (schema->models)->data : NULL;
}

apteryx_schema_model*
apteryx_schema_next_model (apteryx_schema_instance *schema, apteryx_schema_model *model)
{
    GList *node = g_list_find (schema->models, (gconstpointer) model);
    node = node ? g_list_next (node) : NULL;
    return node ? (apteryx_schema_model *)node->data : NULL;
}

const char *
apteryx_schema_model_name (apteryx_schema_model *model)
{
    return model->name;
}

const char *
apteryx_schema_model_organization (apteryx_schema_model *model)
{
    return model->organization;
}

const char *
apteryx_schema_model_version (apteryx_schema_model *model)
{
    return model->version;
}

static gboolean
match_name (const char *s1, const char *s2)
{
    char c1, c2;
    do
    {
        c1 = *s1;
        c2 = *s2;
        if (c1 == '\0' && c2 == '\0')
            return true;
        if (c1 == '-')
            c1 = '_';
        if (c2 == '-')
            c2 = '_';
        s1++;
        s2++;
    } while (c1 == c2);
    return false;
}

static struct apteryx_schema_node *
lookup_node (struct apteryx_schema_node *node, const char *path, int depth)
{
    char *key = NULL;
    int len;
    GList *iter;

    if (!node)
    {
        return NULL;
    }

    if (path[0] == '/')
    {
        path++;
    }
    key = strchr (path, '/');
    if (key)
    {
        len = key - path;
        key = strndup (path, len);
        path += len;
    }
    else
    {
        key = strdup (path);
        path = NULL;
    }

    for (iter = node->children; iter; iter = g_list_next (iter))
    {
        struct apteryx_schema_node *n = (struct apteryx_schema_node *) iter->data;

        DEBUG ("%*sCMP: %s to %s", depth*2, " ", key, n->name);

        if (n->name && (n->name[0] == '*' ||  match_name (n->name, key)))
        {
            DEBUG(" - MATCH\n");
            free (key);
            if (path)
            {
                return lookup_node (n, path, depth+1);
            }
            return n;
        }
        DEBUG(" - NO\n");
    }

    free (key);
    return NULL;
}

apteryx_schema_node *
apteryx_schema_lookup (apteryx_schema_instance *schema, const char *path)
{
    struct apteryx_schema_node *root;
    struct apteryx_schema_node *node;

    DEBUG ("LOOKUP: %s\n", path);

    /* Find root node */
    char *rpath = g_strdup (path + 1);
    char *tmp = strchr (rpath, '/');
    if (tmp)
        *tmp = '\0';

    /* Find the root node */
    root = (struct apteryx_schema_node *) g_hash_table_lookup (schema->roots, rpath);
    if (!root)
    {
        DEBUG ("No root node for %s\n", rpath);
        free (rpath);
        return NULL;
    }

    /* Check if we only want the root */
    if (strlen (path) == (strlen (rpath) + 1))
    {
        node = root;
    }
    else
    {
        /* Recursively find the node */
        node = lookup_node (root, path + strlen (rpath) + 1, 0);
    }

    free (rpath);
    return node;
}

bool
apteryx_schema_is_leaf (apteryx_schema_node *node)
{
    return (node->flags & NODE_FLAGS_LEAF) == NODE_FLAGS_LEAF;
}

bool
apteryx_schema_is_readable (apteryx_schema_node *node)
{
    return (node->flags & NODE_FLAGS_READ) == NODE_FLAGS_READ;
}

bool
apteryx_schema_is_writable (apteryx_schema_node *node)
{
    return (node->flags & NODE_FLAGS_WRITE) == NODE_FLAGS_WRITE;
}

char *
apteryx_schema_translate_to (apteryx_schema_node *node, char *value)
{
    GList *iter;

    /* Get the default if needed - untranslated */
    if (!value && node->defvalue)
    {
        value = g_strdup (node->defvalue);
    }
    
    /* Find an ENUM node with this value */
    for (iter = node->children; iter; iter = g_list_next (iter))
    {
        apteryx_schema_node *n = (apteryx_schema_node *) iter->data;
        if ((n->flags & NODE_FLAGS_ENUM) == NODE_FLAGS_ENUM &&
            g_strcmp0 (value, n->value) == 0)
        {
            free (value);
            value = g_strdup (n->name);
            break;
        }
    }
    return value;
}

char *
apteryx_schema_translate_from (apteryx_schema_node *node, char *value)
{
    GList *iter;

    /* Find an ENUM node with this name */
    for (iter = node->children; iter; iter = g_list_next (iter))
    {
        apteryx_schema_node *n = (apteryx_schema_node *) iter->data;
        if ((n->flags & NODE_FLAGS_ENUM) == NODE_FLAGS_ENUM &&
            g_strcmp0 (value, n->name) == 0)
        {
            free (value);
            value = g_strdup (n->value);
            break;
        }
    }
    return value;
}

char*
apteryx_schema_name (apteryx_schema_node *node)
{
    return node ? g_strdup (node->name) : NULL;
}
