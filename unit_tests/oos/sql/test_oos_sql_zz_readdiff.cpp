/*
 * test_oos_sql_zz_readdiff.cpp - read-equivalence dump harness (adversarial review of CUBRID PR 8299)
 *
 * Deterministically builds tables, runs queries and direct heap-API reads, and writes a canonical text dump of
 * every fetched value (type id, NULL flag, precision, scale, codeset, collation, packed-byte hash) to the file
 * named by OOS_READDIFF_OUT. The same file is built at two revisions and the two dumps are diffed.
 */

#include "test_oos_sql_common.hpp"

#include "log_impl.h"
#include "object_representation.h"
#include "object_primitive.h"
#include "btree.h"
#include "scope_exit.hpp"
#include "work_space.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

// XXX: SHOULD BE THE LAST INCLUDE HEADER
#include "memory_wrapper.hpp"

void bridge_oos_debug_counters_reset ();
oos_debug_counters bridge_oos_debug_counters_get ();

// ============================================================================
// Dump plumbing
// ============================================================================

static FILE *g_out = nullptr;

static void
out (const char *fmt, ...)
{
  if (g_out == nullptr)
    {
      const char *path = getenv ("OOS_READDIFF_OUT");
      g_out = fopen (path != nullptr ? path : "/dev/null", "w");
    }
  va_list ap;
  va_start (ap, fmt);
  vfprintf (g_out, fmt, ap);
  va_end (ap);
  fflush (g_out);
}

static uint64_t
fnv (const void *p, size_t n)
{
  const unsigned char *c = (const unsigned char *) p;
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++)
    {
      h ^= c[i];
      h *= 1099511628211ULL;
    }
  return h;
}

static std::string
dump_value (DB_VALUE *v)
{
  char buf[256];
  const DB_TYPE t = db_value_type (v);
  const bool is_null = db_value_is_null (v) != 0;
  int cs = -1;
  int coll = -1;
  if (t == DB_TYPE_CHAR || t == DB_TYPE_VARCHAR)
    {
      cs = db_get_string_codeset (v);
      coll = db_get_string_collation (v);
    }
  int prec = -2;
  int scale = -2;
  if (t == DB_TYPE_NUMERIC || t == DB_TYPE_CHAR || t == DB_TYPE_VARCHAR || t == DB_TYPE_BIT || t == DB_TYPE_VARBIT)
    {
      prec = db_value_precision (v);
      scale = db_value_scale (v);
    }
  snprintf (buf, sizeof (buf), "{t=%d n=%d p=%d s=%d cs=%d co=%d", (int) t, is_null ? 1 : 0, prec, scale, cs, coll);
  std::string s (buf);
  if (!is_null)
    {
      if (t == DB_TYPE_BLOB || t == DB_TYPE_CLOB)
	{
	  DB_ELO *e = db_get_elo (v);
	  snprintf (buf, sizeof (buf), " elo.type=%d elo.size=%lld", (int) e->type, (long long) db_elo_size (e));
	  s += buf;
	}
      else if (t == DB_TYPE_MIDXKEY)
	{
	  DB_MIDXKEY *mk = db_get_midxkey (v);
	  snprintf (buf, sizeof (buf), " mk.size=%d mk.n=%d h=%016llx", mk->size, mk->ncolumns,
		    (unsigned long long) fnv (mk->buf, (size_t) mk->size));
	  s += buf;
	}
      else
	{
	  const int sz = pr_data_writeval_disk_size (v);
	  if (sz > 0)
	    {
	      std::vector<char> b ((size_t) sz + 16, 0);
	      OR_BUF ob;
	      or_init (&ob, b.data (), sz);
	      pr_data_writeval (&ob, v);
	      snprintf (buf, sizeof (buf), " pk=%d h=%016llx", sz, (unsigned long long) fnv (b.data (), (size_t) sz));
	      s += buf;
	    }
	  else
	    {
	      s += " pk=0";
	    }
	}
    }
  s += "}";
  return s;
}

static void
dump_counters (const char *tag)
{
  const oos_debug_counters c = bridge_oos_debug_counters_get ();
  out ("%s rm_calls=%llu rm_req=%llu rm_head=%llu rm_vpp=%llu\n", tag, c.read_many_calls, c.read_many_requests,
       c.read_many_grouped_head_pages, c.read_values_per_fixed_page);
}

static void
run_sql (const std::string &label, const std::string &sql)
{
  out ("\n## %s | %.300s\n", label.c_str (), sql.c_str ());
  bridge_oos_debug_counters_reset ();
  DB_QUERY_RESULT *res = nullptr;
  DB_QUERY_ERROR qe;
  const int rc = db_compile_and_execute_local (sql.c_str (), &res, &qe);
  if (rc < 0)
    {
      out ("ERR rc=%d code=%d\n", rc, db_error_code ());
      if (res != nullptr)
	{
	  db_query_end (res);
	}
      db_abort_transaction ();
      return;
    }
  if (res == nullptr)
    {
      out ("OK rc=%d (no result)\n", rc);
      return;
    }
  const int ncol = db_query_column_count (res);
  out ("OK rc=%d ncol=%d\n", rc, ncol);
  int cr = db_query_first_tuple (res);
  int row = 0;
  while (cr == DB_CURSOR_SUCCESS)
    {
      std::string line = "R" + std::to_string (row++) + ":";
      for (int i = 0; i < ncol; i++)
	{
	  DB_VALUE v;
	  db_make_null (&v);
	  const int e = db_query_get_tuple_value (res, i, &v);
	  if (e != NO_ERROR)
	    {
	      line += " <getval err " + std::to_string (e) + ">";
	      continue;
	    }
	  line += " " + dump_value (&v);
	  db_value_clear (&v);
	}
      out ("%s\n", line.c_str ());
      cr = db_query_next_tuple (res);
    }
  out ("ROWS=%d end_cursor=%d\n", row, cr);
  dump_counters ("CNT");
  db_query_end (res);
}

static void
run_dml (const std::string &label, const std::string &sql)
{
  bridge_oos_debug_counters_reset ();
  const int rc = exec_sql (sql.c_str ());
  out ("\n## DML %s | %.200s\nrc=%d", label.c_str (), sql.c_str (), rc);
  if (rc < 0)
    {
      out (" code=%d", db_error_code ());
      db_abort_transaction ();
    }
  else
    {
      db_commit_transaction ();
    }
  out ("\n");
}

static void
run_ddl (const std::string &sql)
{
  const int rc = exec_sql (sql.c_str ());
  out ("\n## DDL %.300s\nrc=%d", sql.c_str (), rc);
  if (rc < 0)
    {
      out (" code=%d", db_error_code ());
      db_abort_transaction ();
    }
  else
    {
      db_commit_transaction ();
    }
  out ("\n");
}

static void
drop_table (const std::string &name)
{
  exec_sql (("DROP TABLE IF EXISTS " + name).c_str ());
  db_commit_transaction ();
}

// ============================================================================
// Direct heap-API reads
// ============================================================================

struct row_info
{
  OID oid;
  std::vector<char> bytes;
  int id = 0;
};

struct table_ctx
{
  OID class_oid;
  HFID hfid;
  DB_OBJECT *cls = nullptr;
  std::vector<row_info> rows;
};

static bool
collect_rows (const std::string &table, table_ctx &ctx)
{
  auto *thread = thread_get_thread_entry_info ();
  ctx.cls = db_find_class (table.c_str ());
  if (ctx.cls == nullptr)
    {
      return false;
    }
  ctx.class_oid = *WS_OID (ctx.cls);
  if (heap_get_class_hfid (thread, &ctx.class_oid, &ctx.hfid, nullptr) != NO_ERROR)
    {
      return false;
    }
  DB_OBJLIST *objects = db_fetch_all_objects (ctx.cls, DB_FETCH_READ);
  if (objects == nullptr)
    {
      return true;
    }
  scope_exit free_objects ([&] ()
  {
    db_objlist_free (objects);
  });
  for (DB_OBJLIST *item = objects; item != nullptr; item = item->next)
    {
      row_info ri;
      ri.oid = *WS_OID (item->op);
      HEAP_SCANCACHE scan;
      if (heap_scancache_start (thread, &scan, &ctx.hfid, &ctx.class_oid, true, logtb_get_mvcc_snapshot (thread)) != NO_ERROR)
	{
	  return false;
	}
      RECDES record = RECDES_INITIALIZER;
      OID class_oid = ctx.class_oid;
      const SCAN_CODE status = heap_get_visible_version (thread, &ri.oid, &class_oid, &record, &scan, PEEK, NULL_CHN,
			       HEAP_RECDES_DONT_CONSUME_RAW_BYTES);
      if (status == S_SUCCESS)
	{
	  ri.bytes.assign (record.data, record.data + record.length);
	}
      heap_scancache_end (thread, &scan);
      if (status != S_SUCCESS)
	{
	  continue;
	}
      ctx.rows.push_back (std::move (ri));
    }
  return true;
}

static std::vector<std::pair<std::string, int>>
    list_attrs (DB_OBJECT *cls, bool class_attrs)
{
  std::vector<std::pair<std::string, int>> v;
  DB_ATTRIBUTE *a = class_attrs ? db_get_class_attributes (cls) : db_get_attributes (cls);
  for (; a != nullptr; a = db_attribute_next (a))
    {
      v.emplace_back (db_attribute_name (a), db_attribute_id (a));
    }
  return v;
}

static uint64_t g_rng = 88172645463325252ULL;
static uint64_t
rnd ()
{
  g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
  return g_rng >> 33;
}

static std::vector<std::vector<int>>
				  make_subsets (int n)
{
  std::vector<std::vector<int>> s;
  for (int i = 0; i < n; i++)
    {
      s.push_back ({ i });
    }
  for (int i = 0; i < n; i++)
    {
      for (int j = i + 1; j < n; j++)
	{
	  s.push_back ({ i, j });
	  if (n <= 8)
	    {
	      s.push_back ({ j, i });
	    }
	}
    }
  if (n <= 9)
    {
      for (int i = 0; i < n; i++)
	for (int j = i + 1; j < n; j++)
	  for (int k = j + 1; k < n; k++)
	    {
	      s.push_back ({ i, j, k });
	    }
    }
  else
    {
      for (int r = 0; r < 60; r++)
	{
	  int k = 3 + (int) (rnd () % 6);
	  std::vector<int> p;
	  for (int q = 0; q < n; q++)
	    {
	      p.push_back (q);
	    }
	  for (int q = n - 1; q > 0; q--)
	    {
	      std::swap (p[q], p[rnd () % (q + 1)]);
	    }
	  p.resize (k);
	  s.push_back (p);
	}
    }
  std::vector<int> all;
  for (int i = 0; i < n; i++)
    {
      all.push_back (i);
    }
  s.push_back (all);
  std::vector<int> rev (all.rbegin (), all.rend ());
  s.push_back (rev);
  for (int r = 0; r < 12; r++)
    {
      std::vector<int> p = all;
      for (int q = n - 1; q > 0; q--)
	{
	  std::swap (p[q], p[rnd () % (q + 1)]);
	}
      s.push_back (p);
    }
  // suffix halves: the later attributes only
  if (n >= 4)
    {
      std::vector<int> half (all.begin () + n / 2, all.end ());
      s.push_back (half);
      std::vector<int> head (all.begin (), all.begin () + n / 2);
      s.push_back (head);
    }
  return s;
}


// Canonical, padding-insensitive fingerprint of a heap record: offset width, VOT entries (offset + flags),
// fixed-area bytes of bound attributes only, bound-bit state, and a hash of each variable field (OOS stubs are
// reduced to their full length and head OID).
static std::string
canon_record (const OID &class_oid, RECDES &rec)
{
  auto *thread = thread_get_thread_entry_info ();
  int cache_index = -1;
  OR_CLASSREP *repr = heap_classrepr_get (thread, &class_oid, nullptr, OR_GET_MVCC_REPID (rec.data), &cache_index);
  if (repr == nullptr)
    {
      return "canon=<no repr>";
    }
  scope_exit free_repr ([&] ()
  {
    heap_classrepr_free (repr, &cache_index);
  });
  const int hdr = OR_HEADER_SIZE (rec.data);
  const int osz = OR_GET_OFFSET_SIZE (rec.data);
  const char *var_table = rec.data + hdr;
  const char *fixed_base = var_table + OR_VAR_TABLE_SIZE_INTERNAL (repr->n_variable, osz);
  const char *bits = fixed_base + repr->fixed_length;
  const bool has_bits = OR_GET_BOUND_BIT_FLAG (rec.data) != 0;
  char buf[256];
  snprintf (buf, sizeof (buf), "canon: osz=%d nvar=%d flen=%d bitsflag=%d vot=", osz, repr->n_variable,
	    repr->fixed_length,
	    has_bits ? 1 : 0);
  std::string r (buf);
  std::vector<int> entries;
  for (int i = 0; i <= repr->n_variable; i++)
    {
      int e = 0;
      heap_recdes_get_var_offset_entry (&rec, i, &e);
      entries.push_back (e);
      r += std::to_string (e) + ",";
    }
  // fixed attributes
  std::string fx;
  for (int i = 0; i < repr->n_attributes; i++)
    {
      const OR_ATTRIBUTE &a = repr->attributes[i];
      if (!a.is_fixed)
	{
	  continue;
	}
      const int dsz = tp_domain_disk_size (a.domain);
      const bool bound = !has_bits || OR_GET_BOUND_BIT (bits, a.position) != 0;
      snprintf (buf, sizeof (buf), " f%d(%d,%s)", a.id, dsz, bound ? "B" : "N");
      fx += buf;
      if (bound)
	{
	  snprintf (buf, sizeof (buf), "=%016llx", (unsigned long long) fnv (fixed_base + a.location, (size_t) dsz));
	  fx += buf;
	}
    }
  r += " fixed:" + fx + " var:";
  for (int i = 0; i < repr->n_variable; i++)
    {
      const int this_off = OR_GET_VAR_OFFSET (entries[i]);
      const int next_off = OR_GET_VAR_OFFSET (entries[i + 1]);
      if (OR_IS_OOS (entries[i]))
	{
	  char *stub = nullptr;
	  if (heap_recdes_get_oos_inline_stub (&rec, i, &stub) == NO_ERROR)
	    {
	      snprintf (buf, sizeof (buf), " [%d:OOS oid=%016llx len=%016llx]", i, (unsigned long long) fnv (stub, 8),
			(unsigned long long) fnv (stub + 8, 8));
	    }
	  else
	    {
	      snprintf (buf, sizeof (buf), " [%d:OOS bad stub]", i);
	    }
	}
      else
	{
	  snprintf (buf, sizeof (buf), " [%d:%d=%016llx]", i, next_off - this_off,
		    (unsigned long long) fnv (var_table + this_off, (size_t) (next_off - this_off)));
	}
      r += buf;
    }
  return r;
}

// Read every requested subset of attributes of every row through heap_attrinfo_read_dbvalues{,_without_oid}.
static void
direct_dump (const std::string &label, const std::string &table, bool include_class_attrs = false)
{
  table_ctx ctx;
  out ("\n#### DIRECT %s table=%s\n", label.c_str (), table.c_str ());
  if (!collect_rows (table, ctx))
    {
      out ("collect failed\n");
      return;
    }
  auto *thread = thread_get_thread_entry_info ();
  auto names = list_attrs (ctx.cls, false);
  if (include_class_attrs)
    {
      auto cn = list_attrs (ctx.cls, true);
      names.insert (names.end (), cn.begin (), cn.end ());
    }
  if (names.empty ())
    {
      out ("no attrs\n");
      return;
    }
  // row identity: first integer-ish attribute named id
  int id_attr = -1;
  for (auto &p : names)
    {
      if (p.first == "id")
	{
	  id_attr = p.second;
	}
    }
  if (id_attr >= 0)
    {
      HEAP_CACHE_ATTRINFO ia;
      ATTR_ID ids[1] = { (ATTR_ID) id_attr };
      if (heap_attrinfo_start (thread, &ctx.class_oid, 1, ids, &ia) == NO_ERROR)
	{
	  for (auto &r : ctx.rows)
	    {
	      RECDES rec = RECDES_INITIALIZER;
	      rec.data = r.bytes.data ();
	      rec.length = (int) r.bytes.size ();
	      rec.area_size = rec.length;
	      if (heap_attrinfo_read_dbvalues (thread, &r.oid, &rec, &ia) == NO_ERROR)
		{
		  DB_VALUE *v = heap_attrinfo_access (ids[0], &ia);
		  r.id = (v != nullptr && db_value_type (v) == DB_TYPE_INTEGER && !db_value_is_null (v)) ? db_get_int (v) : -1;
		}
	    }
	  heap_attrinfo_end (thread, &ia);
	}
    }
  std::sort (ctx.rows.begin (), ctx.rows.end (), [] (const row_info &a, const row_info &b)
  {
    return a.id < b.id;
  });
  out ("attrs:");
  for (auto &p : names)
    {
      out (" %s", p.first.c_str ());
    }
  out ("\nrows=%d\n", (int) ctx.rows.size ());
  for (auto &r : ctx.rows)
    {
      RECDES rec = RECDES_INITIALIZER;
      rec.data = r.bytes.data ();
      rec.length = (int) r.bytes.size ();
      rec.area_size = rec.length;
      const int hsz = OR_HEADER_SIZE (rec.data);
      out ("ROW id=%d len=%d hdr=%d contains_oos=%d repid=%d\n", r.id, rec.length, hsz,
	   heap_recdes_contains_oos (&rec) ? 1 : 0, (int) OR_GET_MVCC_REPID (rec.data));
      out ("%s\n", canon_record (ctx.class_oid, rec).c_str ());
    }
  g_rng = 88172645463325252ULL;
  const auto subsets = make_subsets ((int) names.size ());
  int sn = 0;
  for (const auto &sub : subsets)
    {
      std::vector<ATTR_ID> ids;
      std::string nm;
      for (int i : sub)
	{
	  ids.push_back ((ATTR_ID) names[i].second);
	  nm += names[i].first + ",";
	}
      HEAP_CACHE_ATTRINFO ia;
      const int e = heap_attrinfo_start (thread, &ctx.class_oid, (int) ids.size (), ids.data (), &ia);
      out ("\n-- subset#%d [%s] start=%d\n", sn++, nm.c_str (), e);
      if (e != NO_ERROR)
	{
	  continue;
	}
      for (auto &r : ctx.rows)
	{
	  for (int variant = 0; variant < 2; variant++)
	    {
	      RECDES rec = RECDES_INITIALIZER;
	      rec.data = r.bytes.data ();
	      rec.length = (int) r.bytes.size ();
	      rec.area_size = rec.length;
	      bridge_oos_debug_counters_reset ();
	      const int err = variant == 0 ? heap_attrinfo_read_dbvalues (thread, &r.oid, &rec, &ia)
			      : heap_attrinfo_read_dbvalues_without_oid (thread, &rec, &ia);
	      const oos_debug_counters c = bridge_oos_debug_counters_get ();
	      std::string line = "D" + std::to_string (variant) + " id=" + std::to_string (r.id) + " e=" + std::to_string (err)
				 + " cnt=" + std::to_string (c.read_many_calls) + "/" + std::to_string (c.read_many_requests)
				 + ":";
	      if (err == NO_ERROR)
		{
		  for (size_t k = 0; k < ids.size (); k++)
		    {
		      DB_VALUE *v = heap_attrinfo_access (ids[k], &ia);
		      line += v == nullptr ? " <null-ptr>" : " " + dump_value (v);
		    }
		}
	      out ("%s\n", line.c_str ());
	    }
	}
      heap_attrinfo_end (thread, &ia);
    }
}


// Rebuild a record with a different MVCC header size (flags 0..7) and/or a different variable-offset width, keeping
// every field value. Returns false when the offsets do not fit the requested width.
static bool
synth_record (const OID &class_oid, const std::vector<char> &orig, int mvcc_flags, int new_osz,
	      std::vector<char> &outbuf)
{
  auto *thread = thread_get_thread_entry_info ();
  int cache_index = -1;
  const char *d = orig.data ();
  OR_CLASSREP *repr = heap_classrepr_get (thread, &class_oid, nullptr, OR_GET_MVCC_REPID (d), &cache_index);
  if (repr == nullptr)
    {
      return false;
    }
  const int nvars = repr->n_variable;
  heap_classrepr_free (repr, &cache_index);
  const int hdr = OR_HEADER_SIZE (d);
  const int osz = OR_GET_OFFSET_SIZE (d);
  const int old_vot = OR_VAR_TABLE_SIZE_INTERNAL (nvars, osz);
  const int new_vot = OR_VAR_TABLE_SIZE_INTERNAL (nvars, new_osz);
  int word0 = OR_GET_INT (d);
  word0 &= ~OR_OFFSET_SIZE_FLAG;
  OR_SET_VAR_OFFSET_SIZE (word0, new_osz);
  word0 = (word0 & ~ (OR_RECORD_MVCC_FLAG_MASK << OR_RECORD_FLAG_SHIFT_BITS)) | (mvcc_flags << OR_RECORD_FLAG_SHIFT_BITS);
  int bits = 0;
  for (int f = mvcc_flags; f; f >>= 1)
    {
      bits += f & 1;
    }
  const int new_hdr = 8 + 8 * bits;
  const int delta = new_vot - old_vot;
  outbuf.assign ((size_t) new_hdr + (size_t) new_vot + (orig.size () - (size_t) hdr - (size_t) old_vot), 0x5A);
  char *o = outbuf.data ();
  OR_PUT_INT (o, word0);
  memcpy (o + 4, d + 4, 4);
  char *vt = o + new_hdr;
  const char *ovt = d + hdr;
  for (int i = 0; i <= nvars; i++)
    {
      int e = 0;
      switch (osz)
	{
	case OR_BYTE_SIZE:
	  e = OR_GET_BYTE (ovt + i * osz);
	  break;
	case OR_SHORT_SIZE:
	  e = OR_GET_SHORT (ovt + i * osz);
	  break;
	default:
	  e = OR_GET_INT (ovt + i * osz);
	  break;
	}
      const int ne = (OR_GET_VAR_OFFSET (e) + delta) | OR_GET_VAR_FLAG (e);
      switch (new_osz)
	{
	case OR_BYTE_SIZE:
	  if (ne < 0 || ne > 255)
	    {
	      return false;
	    }
	  OR_PUT_BYTE (vt + i * new_osz, ne);
	  break;
	case OR_SHORT_SIZE:
	  if (ne < 0 || ne > 32767)
	    {
	      return false;
	    }
	  OR_PUT_SHORT (vt + i * new_osz, ne);
	  break;
	default:
	  OR_PUT_INT (vt + i * new_osz, ne);
	  break;
	}
    }
  memcpy (o + new_hdr + new_vot, d + hdr + old_vot, orig.size () - (size_t) hdr - (size_t) old_vot);
  return true;
}

// For each row, read every (limited) subset of attributes from the record and from rebuilt copies with every
// MVCC header size and offset width; any value that differs from the original record's is reported.
static void
direct_variants (const std::string &label, const std::string &table)
{
  table_ctx ctx;
  out ("\n#### VARIANTS %s table=%s\n", label.c_str (), table.c_str ());
  if (!collect_rows (table, ctx))
    {
      out ("collect failed\n");
      return;
    }
  auto *thread = thread_get_thread_entry_info ();
  auto names = list_attrs (ctx.cls, false);
  int id_attr = -1;
  for (auto &p : names)
    {
      if (p.first == "id")
	{
	  id_attr = p.second;
	}
    }
  const int n = (int) names.size ();
  std::vector<std::vector<int>> subsets;
  for (int i = 0; i < n; i++)
    {
      subsets.push_back ({ i });
    }
  for (int i = 0; i + 1 < n; i++)
    {
      subsets.push_back ({ i, i + 1 });
    }
  std::vector<int> all;
  for (int i = 0; i < n; i++)
    {
      all.push_back (i);
    }
  subsets.push_back (all);
  subsets.push_back (std::vector<int> (all.rbegin (), all.rend ()));
  g_rng = 99991;
  for (int r = 0; r < 20; r++)
    {
      std::vector<int> pm = all;
      for (int q = n - 1; q > 0; q--)
	{
	  std::swap (pm[q], pm[rnd () % (q + 1)]);
	}
      pm.resize (std::max (2, (int) (rnd () % n)));
      subsets.push_back (pm);
    }
  long long total = 0, bad = 0, skipped = 0;
  int sn = 0;
  for (const auto &sub : subsets)
    {
      std::vector<ATTR_ID> ids;
      for (int i : sub)
	{
	  ids.push_back ((ATTR_ID) names[i].second);
	}
      HEAP_CACHE_ATTRINFO ia;
      if (heap_attrinfo_start (thread, &ctx.class_oid, (int) ids.size (), ids.data (), &ia) != NO_ERROR)
	{
	  continue;
	}
      sn++;
      for (auto &r : ctx.rows)
	{
	  auto read_line = [&] (std::vector<char> &bytes, int variant_kind) -> std::string
	  {
	    RECDES rec = RECDES_INITIALIZER;
	    rec.data = bytes.data ();
	    rec.length = (int) bytes.size ();
	    rec.area_size = rec.length;
	    const int err = variant_kind == 0 ? heap_attrinfo_read_dbvalues (thread, &r.oid, &rec, &ia)
	    : heap_attrinfo_read_dbvalues_without_oid (thread, &rec, &ia);
	    std::string line = "e=" + std::to_string (err) + ":";
	    if (err == NO_ERROR)
	      {
		for (size_t k = 0; k < ids.size (); k++)
		  {
		    DB_VALUE *v = heap_attrinfo_access (ids[k], &ia);
		    line += v == nullptr ? " <null-ptr>" : " " + dump_value (v);
		  }
	      }
	    return line;
	  };
	  std::vector<char> orig = r.bytes;
	  const std::string ref = read_line (orig, 0);
	  out ("V s%d id=%d %s\n", sn, id_attr, ref.c_str ());
	  for (int f = 0; f < 8; f++)
	    {
	      for (int osz :
		   {
			   1, 2, 4
		   })
		{
		  std::vector<char> v;
		  if (!synth_record (ctx.class_oid, r.bytes, f, osz, v))
		    {
		      skipped++;
		      continue;
		    }
		  for (int kind = 0; kind < 2; kind++)
		    {
		      total++;
		      const std::string got = read_line (v, kind);
		      if (got != ref)
			{
			  bad++;
			  out ("MISMATCH s%d f=%d osz=%d kind=%d\n  ref=%s\n  got=%s\n", sn, f, osz, kind, ref.c_str (), got.c_str ());
			}
		    }
		}
	    }
	}
      heap_attrinfo_end (thread, &ia);
    }
  out ("VARIANT SUMMARY %s: subsets=%d reads=%lld mismatches=%lld skipped=%lld\n", label.c_str (), sn, total, bad,
       skipped);
}


// Attribute lists that repeat the same attribute (same OOS head chunk requested several times in one grouped read).
static void
direct_dups (const std::string &label, const std::string &table, const std::vector<std::vector<std::string>> &lists)
{
  table_ctx ctx;
  out ("\n#### DUPS %s table=%s\n", label.c_str (), table.c_str ());
  if (!collect_rows (table, ctx))
    {
      out ("collect failed\n");
      return;
    }
  auto *thread = thread_get_thread_entry_info ();
  auto names = list_attrs (ctx.cls, false);
  std::map<std::string, int> idmap;
  for (auto &p : names)
    {
      idmap[p.first] = p.second;
    }
  int sn = 0;
  for (const auto &list : lists)
    {
      std::vector<ATTR_ID> ids;
      std::string nm;
      for (const auto &n : list)
	{
	  ids.push_back ((ATTR_ID) idmap[n]);
	  nm += n + ",";
	}
      HEAP_CACHE_ATTRINFO ia;
      const int e = heap_attrinfo_start (thread, &ctx.class_oid, (int) ids.size (), ids.data (), &ia);
      out ("\n-- dups#%d [%s] start=%d\n", sn++, nm.c_str (), e);
      if (e != NO_ERROR)
	{
	  continue;
	}
      for (auto &r : ctx.rows)
	{
	  for (int pass = 0; pass < 2; pass++)
	    {
	      RECDES rec = RECDES_INITIALIZER;
	      rec.data = r.bytes.data ();
	      rec.length = (int) r.bytes.size ();
	      rec.area_size = rec.length;
	      bridge_oos_debug_counters_reset ();
	      const int err = pass == 0 ? heap_attrinfo_read_dbvalues (thread, &r.oid, &rec, &ia)
			      : heap_attrinfo_read_dbvalues_without_oid (thread, &rec, &ia);
	      const oos_debug_counters c = bridge_oos_debug_counters_get ();
	      std::string line = "X" + std::to_string (pass) + " e=" + std::to_string (err) + " cnt=" + std::to_string (
					 c.read_many_calls) + "/"
				 + std::to_string (c.read_many_requests) + ":";
	      if (err == NO_ERROR)
		{
		  for (size_t k = 0; k < ids.size (); k++)
		    {
		      DB_VALUE *v = heap_attrinfo_access (ids[k], &ia);
		      line += v == nullptr ? " <null-ptr>" : " " + dump_value (v);
		    }
		}
	      out ("%s\n", line.c_str ());
	    }
	}
      heap_attrinfo_end (thread, &ia);
    }
}

// Build the key of every index of the class for every row (heap_midxkey_get_value path).
static void
direct_keys (const std::string &label, const std::string &table)
{
  table_ctx ctx;
  out ("\n#### KEYS %s table=%s\n", label.c_str (), table.c_str ());
  if (!collect_rows (table, ctx))
    {
      out ("collect failed\n");
      return;
    }
  auto *thread = thread_get_thread_entry_info ();
  auto names = list_attrs (ctx.cls, false);
  int id_attr = -1;
  for (auto &p : names)
    {
      if (p.first == "id")
	{
	  id_attr = p.second;
	}
    }
  if (id_attr >= 0)
    {
      HEAP_CACHE_ATTRINFO ia;
      ATTR_ID ids[1] = { (ATTR_ID) id_attr };
      if (heap_attrinfo_start (thread, &ctx.class_oid, 1, ids, &ia) == NO_ERROR)
	{
	  for (auto &r : ctx.rows)
	    {
	      RECDES rec = RECDES_INITIALIZER;
	      rec.data = r.bytes.data ();
	      rec.length = (int) r.bytes.size ();
	      rec.area_size = rec.length;
	      if (heap_attrinfo_read_dbvalues (thread, &r.oid, &rec, &ia) == NO_ERROR)
		{
		  DB_VALUE *v = heap_attrinfo_access (ids[0], &ia);
		  r.id = (v != nullptr && !db_value_is_null (v)) ? db_get_int (v) : -1;
		}
	    }
	  heap_attrinfo_end (thread, &ia);
	}
    }
  std::sort (ctx.rows.begin (), ctx.rows.end (), [] (const row_info &a, const row_info &b)
  {
    return a.id < b.id;
  });
  HEAP_CACHE_ATTRINFO idx_attrinfo;
  HEAP_IDX_ELEMENTS_INFO idx_info;
  OID class_oid = ctx.class_oid;
  const int found = heap_attrinfo_start_with_index (thread, &class_oid, NULL, &idx_attrinfo, &idx_info);
  out ("found=%d num_btids=%d single=%d multi=%d\n", found, idx_info.num_btids, idx_info.has_single_col ? 1 : 0,
       idx_info.has_multi_col ? 1 : 0);
  if (found <= 0)
    {
      return;
    }
  char buf[DBVAL_BUFSIZE + MAX_ALIGNMENT];
  char *aligned_buf = PTR_ALIGN (buf, MAX_ALIGNMENT);
  for (auto &r : ctx.rows)
    {
      RECDES rec = RECDES_INITIALIZER;
      rec.data = r.bytes.data ();
      rec.length = (int) r.bytes.size ();
      rec.area_size = rec.length;
      if (idx_info.has_single_col)
	{
	  const int e = heap_attrinfo_read_dbvalues (thread, &r.oid, &rec, &idx_attrinfo);
	  if (e != NO_ERROR)
	    {
	      out ("K id=%d read_dbvalues err=%d\n", r.id, e);
	      continue;
	    }
	}
      for (int i = 0; i < idx_info.num_btids; i++)
	{
	  OR_INDEX *index = & (idx_attrinfo.last_classrepr->indexes[i]);
	  if (index->func_index_info != NULL || index->filter_predicate != NULL)
	    {
	      out ("K id=%d idx=%d skipped(func/filter)\n", r.id, i);
	      continue;
	    }
	  BTID btid;
	  DB_VALUE dbvalue;
	  db_make_null (&dbvalue);
	  bridge_oos_debug_counters_reset ();
	  DB_VALUE *key = heap_attrvalue_get_key (thread, i, &idx_attrinfo, &rec, &btid, &dbvalue, aligned_buf, NULL, NULL);
	  const oos_debug_counters c = bridge_oos_debug_counters_get ();
	  if (key == nullptr)
	    {
	      out ("K id=%d idx=%d natts=%d key=NULL err=%d\n", r.id, i, index->n_atts, er_errid ());
	      pr_clear_value (&dbvalue);
	      continue;
	    }
	  out ("K id=%d idx=%d natts=%d cnt=%llu/%llu %s\n", r.id, i, index->n_atts, c.read_many_calls,
	       c.read_many_requests, dump_value (key).c_str ());
	  pr_clear_value (&dbvalue);
	}
    }
  heap_attrinfo_end (thread, &idx_attrinfo);
}

// ============================================================================
// Literal builders
// ============================================================================

static std::string
rnd_text (size_t len, uint64_t seed)
{
  static const char alnum[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  std::string r (len, 'x');
  uint64_t x = seed * 6364136223846793005ULL + 1442695040888963407ULL;
  for (size_t i = 0; i < len; i++)
    {
      x = x * 6364136223846793005ULL + 1442695040888963407ULL;
      r[i] = alnum[ (x >> 33) % 62];
    }
  return r;
}

// Deterministic incompressible text literal (REPEAT() data would be LZ4-compressed to a few bytes on disk).
static std::string
rep (const char *unit, int times, const char *prefix = "")
{
  if (times <= 0)
    {
      return std::string (prefix) + "''";
    }
  const size_t len = strlen (unit) * (size_t) times;
  return std::string (prefix) + "'" + rnd_text (len, fnv (unit, strlen (unit)) ^ (uint64_t) times) + "'";
}

static const int KIND_LEN[6] = { 0, 0, 10, 120, 3000, 18000 };

static std::string
chars_for (int salt)
{
  static const char *u[] = { "ab", "xy", "Zq", "07", "k_" };
  return u[salt % 5];
}

struct TypeSpec
{
  const char *tag;
  const char *type;
  bool stringy;
  std::string (*lit) (int kind, int salt);
};

static std::string
lit_varchar (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  if (kind == 1)
    {
      return "''";
    }
  if (kind == 2)
    {
      return "'short-" + std::to_string (salt) + "'";
    }
  if (salt % 3 == 0 && kind == 3)
    {
      return "REPEAT('" + std::string ("\xea\xb0\x80\xeb\x82\x98x") + "', 20)";
    }
  return rep (chars_for (salt).c_str (), KIND_LEN[kind] / 2);
}

static std::string
lit_iso (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  if (kind == 1)
    {
      return "_iso88591''";
    }
  return rep (chars_for (salt).c_str (), KIND_LEN[kind] / 2, "_iso88591");
}

static std::string
lit_char10 (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  if (kind == 1)
    {
      return "''";
    }
  return "'c" + std::to_string (salt % 100) + "'";
}

static std::string
lit_char1000 (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  if (kind == 1)
    {
      return "''";
    }
  return rep (chars_for (salt).c_str (), std::min (KIND_LEN[kind], 990) / 2);
}

static std::string
lit_nvarchar (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  if (kind == 1)
    {
      return "N''";
    }
  return rep (chars_for (salt).c_str (), KIND_LEN[kind] / 2, "N");
}

static std::string
lit_nchar (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  if (kind == 1)
    {
      return "N''";
    }
  return "N'nc" + std::to_string (salt % 1000) + "'";
}

static std::string
lit_varbit (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  if (kind == 1)
    {
      return "X''";
    }
  static const char *hx[] = { "AB", "5C", "00", "FF", "13" };
  return "REPEAT(X'" + std::string (hx[salt % 5]) + "', " + std::to_string (KIND_LEN[kind]) + ")";
}

static std::string
lit_numeric (int kind, int salt)
{
  static const char *v[] = { "0", "1", "-12345.6789012345", "99999999999999999999999999.9999999999",
			     "-0.0000000001", "31415926535.8979323846"
			   };
  if (kind == 0)
    {
      return "NULL";
    }
  (void) salt;
  return "CAST(" + std::string (v[kind]) + " AS NUMERIC(38,10))";
}

static std::string
lit_set_vc (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  if (kind == 1)
    {
      return "{}";
    }
  if (kind == 2)
    {
      return "{'a" + std::to_string (salt) + "','b'}";
    }
  if (kind == 3)
    {
      return "{" + rep ("p", 60) + ", " + rep ("q", 60) + "}";
    }
  if (kind == 4)
    {
      return "{" + rep ("p", 1500) + ", " + rep ("q", 1500) + ", 'z'}";
    }
  return "{" + rep ("p", 4000) + ", " + rep ("q", 4000) + ", " + rep ("r", 4000) + "}";
}

static std::string
lit_set_int (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  if (kind == 1)
    {
      return "{}";
    }
  int n = kind == 2 ? 3 : kind == 3 ? 12 : kind == 4 ? 800 : 3000;
  std::string s = "{";
  for (int i = 0; i < n; i++)
    {
      s += (i ? "," : "") + std::to_string ((i * 7 + salt) % 100000);
    }
  return s + "}";
}

static std::string
lit_json (int kind, int salt)
{
  (void) salt;
  switch (kind)
    {
    case 0:
      return "NULL";
    case 1:
      return "CAST('{}' AS JSON)";
    case 2:
      return "CAST('{\"a\":1}' AS JSON)";
    case 3:
      return "JSON_OBJECT('k', " + rep ("v", 100) + ")";
    case 4:
      return "JSON_OBJECT('k', " + rep ("v", 3000) + ", 'n', 12)";
    default:
      return "JSON_OBJECT('a', " + rep ("x", 9000) + ", 'b', JSON_ARRAY(1,2,3), 'c', " + rep ("y", 9000) + ")";
    }
}

static std::string
lit_blob (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  static const char *hx[] = { "", "AB", "ABCD", "0102030405", "FFEEDDCC", "AABBCCDD00" };
  (void) salt;
  return "BIT_TO_BLOB(X'" + std::string (hx[kind]) + "')";
}

static std::string
lit_clob (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  (void) salt;
  return "CHAR_TO_CLOB('" + std::string (std::max (1, kind * 3), 'c') + "')";
}

static std::string
lit_enum (int kind, int salt)
{
  (void) salt;
  static const char *v[] = { "NULL", "'red'", "'green'", "'blue'", "'red'", "'blue'" };
  return v[kind];
}

static std::string
lit_set_dt (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  if (kind == 1)
    {
      return "{}";
    }
  int n = kind == 2 ? 2 : kind == 3 ? 6 : kind == 4 ? 80 : 400;
  std::string r = "{";
  for (int i = 0; i < n; i++)
    {
      char b[64];
      snprintf (b, sizeof (b), "%sDATETIME'2021-%02d-%02d 10:%02d:%02d.%03d'", i ? "," : "", 1 + (i + salt) % 12,
		1 + (i * 3) % 28, i % 60,
		(i * 7) % 60, (i * 13) % 1000);
      r += b;
    }
  return r + "}";
}

static std::string
lit_seq_dbl (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  if (kind == 1)
    {
      return "{}";
    }
  int n = kind == 2 ? 2 : kind == 3 ? 10 : kind == 4 ? 300 : 1500;
  std::string r = "{";
  for (int i = 0; i < n; i++)
    {
      r += (i ? "," : "") + std::to_string ((i * 31 + salt) % 977) + "." + std::to_string ((i * 17) % 100);
    }
  return r + "}";
}

static std::string
lit_ms_num (int kind, int salt)
{
  if (kind == 0)
    {
      return "NULL";
    }
  if (kind == 1)
    {
      return "{}";
    }
  int n = kind == 2 ? 2 : kind == 3 ? 8 : kind == 4 ? 200 : 900;
  std::string r = "{";
  for (int i = 0; i < n; i++)
    {
      r += (i ? "," : "") + std::string ("CAST(") + std::to_string ((i * 53 + salt) % 100000) + "." + std::to_string ((
		   i * 7) % 1000) + " AS NUMERIC(12,3))";
    }
  return r + "}";
}

static const TypeSpec TYPES[] =
{
  { "vc", "VARCHAR(40000)", true, lit_varchar },
  { "vcci", "VARCHAR(40000) COLLATE utf8_en_ci", true, lit_varchar },
  { "iso", "VARCHAR(40000) CHARSET iso88591 COLLATE iso88591_bin", true, lit_iso },
  { "ch10", "CHAR(10)", true, lit_char10 },
  { "ch1000", "CHAR(1000)", true, lit_char1000 },
  { "nvc", "NCHAR VARYING(40000)", true, lit_nvarchar },
  { "nch", "NCHAR(30)", true, lit_nchar },
  { "vbit", "BIT VARYING(3000000)", true, lit_varbit },
  { "num", "NUMERIC(38,10)", false, lit_numeric },
  { "setvc", "SET(VARCHAR(5000))", false, lit_set_vc },
  { "msetvc", "MULTISET(VARCHAR(5000))", false, lit_set_vc },
  { "seqvc", "SEQUENCE(VARCHAR(5000))", false, lit_set_vc },
  { "setint", "SET(INT)", false, lit_set_int },
  { "setdt", "SET(DATETIME)", false, lit_set_dt },
  { "seqdbl", "SEQUENCE(DOUBLE)", false, lit_seq_dbl },
  { "msnum", "MULTISET(NUMERIC(12,3))", false, lit_ms_num },
  { "json", "JSON", false, lit_json },
  { "blob", "BLOB", false, lit_blob },
  { "clob", "CLOB", false, lit_clob },
  { "enum", "ENUM('red','green','blue')", false, lit_enum },
};

// ============================================================================
// Tests
// ============================================================================

class ReadDiff : public ::testing::Test
{
};

static void
standard_selects (const std::string &tn, const TypeSpec &ts, const std::string &tag)
{
  const char *cols[] = { "id", "fx", "v_a", "v_b", "fb", "v_c", "tail", "v_d", "fdt" };
  run_sql (tag + " all", "SELECT * FROM " + tn + " ORDER BY id");
  for (const char *c : cols)
    {
      run_sql (tag + " col1 " + c, std::string ("SELECT ") + c + " FROM " + tn);
      run_sql (tag + " col id+" + c, std::string ("SELECT id, ") + c + " FROM " + tn + " ORDER BY id");
    }
  const char *vcols[] = { "v_a", "v_b", "v_c", "v_d" };
  for (int i = 0; i < 4; i++)
    for (int j = i + 1; j < 4; j++)
      {
	run_sql (tag + " pair", std::string ("SELECT ") + vcols[i] + ", " + vcols[j] + " FROM " + tn);
	run_sql (tag + " pairrev", std::string ("SELECT ") + vcols[j] + ", " + vcols[i] + " FROM " + tn);
      }
  run_sql (tag + " triple", "SELECT v_b, v_c, v_d FROM " + tn);
  run_sql (tag + " triple2", "SELECT v_a, tail, v_b, fb, v_c FROM " + tn);
  run_sql (tag + " pred1", "SELECT id FROM " + tn + " WHERE v_b IS NULL ORDER BY id");
  run_sql (tag + " pred2", "SELECT id FROM " + tn + " WHERE v_b IS NOT NULL AND v_c IS NOT NULL ORDER BY id");
  run_sql (tag + " pred3", "SELECT id FROM " + tn + " WHERE id > 3 AND v_b = v_c ORDER BY id");
  run_sql (tag + " pred4", "SELECT id, v_a FROM " + tn + " WHERE fx > 20 AND v_d IS NOT NULL ORDER BY id");
  run_sql (tag + " pred5", "SELECT COUNT(*) FROM " + tn
	   + " WHERE v_a IS NOT NULL AND v_b IS NOT NULL AND v_c IS NOT NULL AND v_d IS NOT NULL");
  run_sql (tag + " pred6", "SELECT id, v_c FROM " + tn + " WHERE fb >= 0 AND v_a = v_d ORDER BY id");
  run_sql (tag + " pred7", "SELECT id, v_b, v_c FROM " + tn + " WHERE v_d IS NULL OR v_a IS NULL ORDER BY id");
  if (ts.stringy)
    {
      run_sql (tag + " len1", "SELECT id, LENGTH(v_a), LENGTH(v_b), LENGTH(v_c), LENGTH(v_d) FROM " + tn + " ORDER BY id");
      run_sql (tag + " len2", "SELECT id FROM " + tn + " WHERE LENGTH(v_b) > 100 AND LENGTH(v_c) < 5000 ORDER BY id");
      run_sql (tag + " like", "SELECT id, v_a FROM " + tn + " WHERE v_b LIKE '%ab%' OR v_c LIKE '%xy%' ORDER BY id");
      run_sql (tag + " lenfirst", "SELECT id FROM " + tn + " WHERE LENGTH(v_d) > 0 AND v_b IS NOT NULL ORDER BY id");
    }
  run_sql (tag + " group", "SELECT v_b, COUNT(*) FROM " + tn + " GROUP BY v_b ORDER BY 1");
  run_sql (tag + " distinct", "SELECT DISTINCT v_c FROM " + tn + " ORDER BY 1");
  run_sql (tag + " orderbycol", "SELECT id, v_b FROM " + tn + " ORDER BY v_b, id");
  run_sql (tag + " join", "SELECT a.id, b.id FROM " + tn + " a, " + tn +
	   " b WHERE a.v_b = b.v_c AND a.id < b.id ORDER BY 1, 2");
  run_sql (tag + " subq", "SELECT id FROM " + tn + " t WHERE EXISTS (SELECT 1 FROM " + tn
	   + " u WHERE u.v_a = t.v_d AND u.id <> t.id) ORDER BY id");
  run_sql (tag + " forupdate", "SELECT id, v_b, v_c FROM " + tn + " WHERE id > 2 AND v_b IS NOT NULL FOR UPDATE");
  db_commit_transaction ();
}

TEST_F (ReadDiff, TypeMatrix)
{
  for (const TypeSpec &ts : TYPES)
    {
      const std::string tn = std::string ("t_rd_") + ts.tag;
      drop_table (tn);
      out ("\n\n################ TYPE %s %s\n", ts.tag, ts.type);
      const std::string T = ts.type;
      const std::string FO = std::string (ts.tag) == "enum" ? "" : " STORAGE FORCE_OUTLINE";
      run_ddl ("CREATE TABLE " + tn + " (id INT, fx INT, v_a " + T + ", v_b " + T + FO + ", fb BIGINT, v_c "
	       + T + FO + ", tail VARCHAR(40), v_d " + T + ", fdt DATETIME)");
      const int rows = 18;
      for (int r = 0; r < rows; r++)
	{
	  int ka, kb, kc, kd;
	  if (r < 12)
	    {
	      ka = r % 6;
	      kb = (r + 1) % 6;
	      kc = (r + 2) % 6;
	      kd = (r + 3) % 6;
	    }
	  else if (r == 12)
	    {
	      ka = kb = kc = kd = 4;
	    }
	  else if (r == 13)
	    {
	      ka = kb = kc = kd = 5;
	    }
	  else if (r == 14)
	    {
	      ka = kb = kc = kd = 0;
	    }
	  else if (r == 15)
	    {
	      ka = kb = kc = kd = 1;
	    }
	  else if (r == 16)
	    {
	      ka = 2;
	      kb = 5;
	      kc = 2;
	      kd = 5;
	    }
	  else
	    {
	      ka = 5;
	      kb = 2;
	      kc = 5;
	      kd = 2;
	    }
	  std::string sql = "INSERT INTO " + tn + " (id, fx, v_a, v_b, fb, v_c, tail, v_d, fdt) VALUES (" + std::to_string (
				    r) + ", "
			    + std::to_string (r * 7) + ", " + ts.lit (ka, r) + ", " + ts.lit (kb, r + 1) + ", "
			    + std::to_string ((long long) r * 1000000007LL) + ", " + ts.lit (kc, r + 2) + ", "
			    + (r % 5 == 0 ? std::string ("NULL") : "'tail" + std::to_string (r) + "'") + ", " + ts.lit (kd, r + 3)
			    + ", DATETIME'2020-03-" + (r + 1 < 10 ? "0" : "") + std::to_string (r + 1) + " 10:20:30.123')";
	  run_dml ("insert " + std::to_string (r), sql);
	}
      out ("oos_pages=%d\n", 0);
      standard_selects (tn, ts, std::string (ts.tag) + " base");
      direct_dump (std::string (ts.tag) + " base", tn);
      direct_variants (std::string (ts.tag) + " base", tn);

      run_dml ("U1", "UPDATE " + tn + " SET fx = fx + 1 WHERE id % 2 = 0");
      run_dml ("U2", "UPDATE " + tn + " SET v_b = v_c WHERE id % 3 = 0");
      run_dml ("U3", "UPDATE " + tn + " SET v_a = v_d, v_d = NULL WHERE id % 4 = 1");
      run_sql (std::string (ts.tag) + " after U123", "SELECT * FROM " + tn + " ORDER BY id");
      run_sql (std::string (ts.tag) + " after U123 cols", "SELECT v_b, v_c, v_a FROM " + tn);
      direct_dump (std::string (ts.tag) + " after U123", tn);
      run_dml ("U4", "UPDATE " + tn + " SET fx = -fx");
      run_dml ("D1", "DELETE FROM " + tn + " WHERE id % 5 = 2");
      run_sql (std::string (ts.tag) + " after D1", "SELECT * FROM " + tn + " ORDER BY id");
      out ("vacuum=%d\n", run_vacuum ());
      run_sql (std::string (ts.tag) + " after vacuum", "SELECT * FROM " + tn + " ORDER BY id");
      direct_dump (std::string (ts.tag) + " after vacuum", tn);
      out ("oos_pages=%d\n", 0);
      drop_table (tn);
    }
}

TEST_F (ReadDiff, WideTable)
{
  const std::string tn = "t_rd_wide";
  drop_table (tn);
  std::string ddl = "CREATE TABLE " + tn + " (id INT, f1 INT, f2 BIGINT, f3 DOUBLE";
  for (int i = 1; i <= 12; i++)
    {
      ddl += ", s" + std::to_string (i) + " VARCHAR(40000) STORAGE FORCE_OUTLINE";
      if (i == 4)
	{
	  ddl += ", n1 NUMERIC(20,5), p1 VARCHAR(100)";
	}
    }
  ddl += ", q1 VARCHAR(200), f4 SMALLINT, en ENUM('red','green','blue'), dt DATE)";
  run_ddl (ddl);
  auto S = [] (int len, int salt)
  {
    return len < 0 ? std::string ("NULL") : len == 0 ? std::string ("''") : rep (chars_for (salt).c_str (), (len + 1) / 2);
  };
  // lens[i] for s1..s12, -1 NULL, 0 empty
  const std::vector<std::vector<int>> plans =
  {
    { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { -1, -1, 500, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    { 300, -1, -1, -1, -1, -1, 700, -1, -1, -1, -1, -1 },
    { 30, 100, 3000, 5000, 70, 31, 9000, 25, 26, 27, 20000, 30000 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 },
    { 0, -1, 400, 0, -1, 600, 0, -1, 800, 0, -1, 1000 },
    { 200, 200, 200, 200, 200, 200, 200, 200, 200, 5, 5, 5 },
    { 200, 200, 200, 200, 200, 200, 200, 200, 5, 5, 5, 5 },
    { 18000, 18000, 18000, 18000, 18000, 18000, 18000, 18000, 18000, 18000, 18000, 18000 },
    { 400, 0, 400, -1, 400, 5, 400, 0, 400, -1, 400, 5 },
    { 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35 },
  };
  int id = 0;
  for (const auto &p : plans)
    {
      std::string sql = "INSERT INTO " + tn + " VALUES (" + std::to_string (id) + ", " + std::to_string (id * 3) + ", "
			+ std::to_string (id * 100000000LL) + ", " + std::to_string (id) + ".25";
      for (int i = 0; i < 12; i++)
	{
	  sql += ", " + S (p[i], id + i);
	  if (i == 3)
	    {
	      sql += ", " + std::string (id % 2 ? "NULL" : "CAST(" + std::to_string (id) + ".5 AS NUMERIC(20,5))") + ", 'p" +
		     std::to_string (id) + "'";
	    }
	}
      sql += ", " + std::string (id % 3 ? "'q'" : "NULL") + ", " + std::to_string (id) + ", " + std::string (
		     id % 4 == 0 ? "NULL" : id % 4 == 1 ? "'red'" : id % 4 == 2 ? "'green'" : "'blue'") + ", DATE'2021-02-" +
	     std::to_string (10 + id) + "')";
      run_dml ("wide insert " + std::to_string (id), sql);
      id++;
    }
  out ("oos_pages=%d\n", 0);
  run_sql ("wide all", "SELECT * FROM " + tn + " ORDER BY id");
  for (int i = 1; i <= 12; i++)
    {
      run_sql ("wide s" + std::to_string (i), "SELECT s" + std::to_string (i) + " FROM " + tn);
      run_sql ("wide id,s" + std::to_string (i), "SELECT id, s" + std::to_string (i) + " FROM " + tn + " ORDER BY id");
    }
  for (int i = 1; i < 12; i++)
    {
      run_sql ("wide pair" + std::to_string (i),
	       "SELECT s" + std::to_string (i) + ", s" + std::to_string (i + 1) + " FROM " + tn);
    }
  run_sql ("wide s1..s8", "SELECT s1,s2,s3,s4,s5,s6,s7,s8 FROM " + tn);
  run_sql ("wide s1..s9", "SELECT s1,s2,s3,s4,s5,s6,s7,s8,s9 FROM " + tn);
  run_sql ("wide s4..s12", "SELECT s4,s5,s6,s7,s8,s9,s10,s11,s12 FROM " + tn);
  run_sql ("wide s12..s1", "SELECT s12,s11,s10,s9,s8,s7,s6,s5,s4,s3,s2,s1 FROM " + tn);
  run_sql ("wide mixed", "SELECT s2, f1, s9, n1, s5, p1, s12, q1, s1, f4 FROM " + tn);
  run_sql ("wide pred lazy", "SELECT id, s3 FROM " + tn +
	   " WHERE id >= 0 AND s9 IS NOT NULL AND s12 IS NOT NULL ORDER BY id");
  run_sql ("wide pred lazy2", "SELECT id FROM " + tn + " WHERE f1 > 3 AND s2 = s3 OR s7 > s8 ORDER BY id");
  run_sql ("wide pred lazy3", "SELECT id, s1, s11 FROM " + tn +
	   " WHERE f1 >= 0 AND LENGTH(s6) > 3 AND LENGTH(s10) > 3 ORDER BY id");
  run_sql ("wide len", "SELECT id, LENGTH(s1), LENGTH(s6), LENGTH(s12) FROM " + tn + " ORDER BY id");
  run_sql ("wide forupdate", "SELECT id, s1, s2, s3 FROM " + tn + " WHERE id > 1 FOR UPDATE");
  db_commit_transaction ();
  direct_dump ("wide", tn);
  direct_variants ("wide", tn);
  direct_dups ("wide", tn,
  {
    { "s1", "s1" }, { "s3", "s3", "s3" }, { "s1", "s2", "s1" },
    { "s1", "s1", "s1", "s1", "s1", "s1", "s1", "s1", "s1", "s1" },
    { "s4", "s5", "s4", "s5", "s4", "s5", "s4", "s5", "s4", "s5", "s4", "s5" },
    { "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "s12", "s1", "s12" },
    { "id", "id", "f1", "f1", "s2", "s2" },
  });
  run_dml ("wide U1", "UPDATE " + tn + " SET f1 = f1 + 1, q1 = 'upd' WHERE id % 2 = 1");
  run_dml ("wide U2", "UPDATE " + tn + " SET s3 = s4, s11 = s12 WHERE id % 3 = 0");
  run_dml ("wide U3", "UPDATE " + tn + " SET s1 = NULL, s2 = '', s5 = " + rep ("m", 1000) + " WHERE id % 4 = 2");
  run_sql ("wide after U", "SELECT * FROM " + tn + " ORDER BY id");
  direct_dump ("wide after U", tn);
  run_dml ("wide D", "DELETE FROM " + tn + " WHERE id % 5 = 1");
  run_sql ("wide after D", "SELECT * FROM " + tn + " ORDER BY id");
  out ("oos_pages=%d\n", 0);
  drop_table (tn);
}

static void
evo_step (const std::string &tn, const std::string &tag, const std::string &ecol = "e", const std::string &dcol = "d")
{
  run_sql (tag + " all", "SELECT * FROM " + tn + " ORDER BY id");
  run_sql (tag + " a,e", "SELECT a, " + ecol + " FROM " + tn);
  run_sql (tag + " e,a", "SELECT " + ecol + ", a FROM " + tn);
  if (!dcol.empty ())
    {
      run_sql (tag + " d", "SELECT " + dcol + " FROM " + tn);
      run_sql (tag + " a,d,e", "SELECT a, " + dcol + ", " + ecol + " FROM " + tn);
    }
  run_sql (tag + " id,a", "SELECT id, a FROM " + tn + " ORDER BY id");
  run_sql (tag + " pred", "SELECT id FROM " + tn + " WHERE a IS NOT NULL AND " + ecol + " IS NOT NULL ORDER BY id");
  run_sql (tag + " pred2", "SELECT id, a FROM " + tn + " WHERE id >= 0 AND " + ecol +
	   " IS NOT NULL AND a IS NOT NULL ORDER BY id");
  direct_dump (tag, tn);
}

TEST_F (ReadDiff, SchemaEvolution)
{
  const std::string tn = "t_rd_evo";
  drop_table (tn);
  run_ddl ("CREATE TABLE " + tn + " (id INT, a VARCHAR(5000) STORAGE FORCE_OUTLINE, b INT, c VARCHAR(100), "
	   "d BIT VARYING(200000) STORAGE FORCE_OUTLINE, e VARCHAR(5000) STORAGE FORCE_OUTLINE, f DOUBLE)");
  auto ins = [&] (int id)
  {
    const int k = id % 5;
    std::string a = k == 0 ? "NULL" : k == 1 ? "''" : rep ("ab", 50 + id * 40);
    std::string c = k == 2 ? "NULL" : "'c" + std::to_string (id) + "'";
    std::string d = k == 3 ? "NULL" : "REPEAT(X'AB', " + std::to_string (30 + id * 50) + ")";
    std::string e = k == 4 ? "NULL" : rep ("xy", 40 + id * 30);
    run_dml ("evo insert " + std::to_string (id),
	     "INSERT INTO " + tn + " (id, a, b, c, d, e, f) VALUES (" + std::to_string (id) + ", " + a + ", " + std::to_string (
		     id * 11)
	     + ", " + c + ", " + d + ", " + e + ", " + std::to_string (id) + ".5)");
  };
  for (int i = 0; i < 10; i++)
    {
      ins (i);
    }
  evo_step (tn, "evo S0");
  run_ddl ("ALTER TABLE " + tn + " ADD COLUMN g INT DEFAULT 41");
  evo_step (tn, "evo S1 add int default");
  run_sql ("evo S1 g", "SELECT g, a FROM " + tn + " ORDER BY id");
  run_ddl ("ALTER TABLE " + tn + " ADD COLUMN h VARCHAR(100) DEFAULT 'a default value longer than twenty four'");
  evo_step (tn, "evo S2 add vc default");
  run_ddl ("ALTER TABLE " + tn +
	   " ADD COLUMN i VARCHAR(5000) DEFAULT 'zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz' STORAGE FORCE_OUTLINE");
  evo_step (tn, "evo S3 add forced default");
  run_sql ("evo S3 h,i,a", "SELECT h, i, a, g FROM " + tn + " ORDER BY id");
  for (int i = 10; i < 16; i++)
    {
      ins (i);
    }
  evo_step (tn, "evo S4 more rows");
  direct_variants ("evo S4", tn);
  run_ddl ("ALTER TABLE " + tn + " DROP COLUMN c");
  evo_step (tn, "evo S5 drop c");
  run_ddl ("ALTER TABLE " + tn + " MODIFY a VARCHAR(8000) STORAGE FORCE_OUTLINE");
  evo_step (tn, "evo S6 widen a");
  run_ddl ("ALTER TABLE " + tn + " MODIFY b BIGINT");
  evo_step (tn, "evo S7 modify b bigint");
  run_ddl ("ALTER TABLE " + tn + " CHANGE e e2 VARCHAR(5000) STORAGE FORCE_OUTLINE");
  run_sql ("evo S8 e2", "SELECT id, e2, a FROM " + tn + " ORDER BY id");
  run_sql ("evo S8 e2only", "SELECT e2 FROM " + tn);
  direct_dump ("evo S8", tn);
  run_ddl ("ALTER TABLE " + tn + " ADD COLUMN j INT NOT NULL DEFAULT 7");
  run_ddl ("ALTER TABLE " + tn + " ADD COLUMN k VARCHAR(300) NOT NULL DEFAULT 'kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk'");
  evo_step (tn, "evo S9 add not null", "e2");
  run_dml ("evo U1", "UPDATE " + tn + " SET b = b + 1 WHERE id % 3 = 0");
  run_dml ("evo U2", "UPDATE " + tn + " SET a = e2 WHERE id % 4 = 1");
  evo_step (tn, "evo S10 after updates", "e2");
  run_ddl ("ALTER TABLE " + tn + " DROP COLUMN d");
  evo_step (tn, "evo S11 drop d", "e2", "");
  run_ddl ("ALTER TABLE " + tn + " ADD COLUMN d VARCHAR(5000) STORAGE FORCE_OUTLINE");
  evo_step (tn, "evo S12 re-add d", "e2");
  run_sql ("evo S12 d", "SELECT id, d, a, e2, i FROM " + tn + " ORDER BY id");
  run_ddl ("ALTER TABLE " + tn +
	   " ADD COLUMN m BIT VARYING(2000) STORAGE FORCE_OUTLINE DEFAULT X'0102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20'");
  run_sql ("evo S13 m", "SELECT id, m, a FROM " + tn + " ORDER BY id");
  direct_dump ("evo S13", tn);
  run_ddl ("ALTER TABLE " + tn + " MODIFY i VARCHAR(5000) NOT NULL DEFAULT 'q'");
  evo_step (tn, "evo S14 modify default", "e2");
  out ("vacuum=%d\n", run_vacuum ());
  evo_step (tn, "evo S15 after vacuum", "e2");
  drop_table (tn);

  // type change that rewrites rows or converts lazily
  const std::string t2 = "t_rd_evo2";
  drop_table (t2);
  run_ddl ("CREATE TABLE " + t2 +
	   " (id INT, a VARCHAR(5000) STORAGE FORCE_OUTLINE, b VARCHAR(100), c INT, d VARCHAR(5000) STORAGE FORCE_OUTLINE)");
  for (int i = 0; i < 8; i++)
    {
      run_dml ("evo2 ins", "INSERT INTO " + t2 + " VALUES (" + std::to_string (i) + ", " + rep ("ab",
	       60 + i * 100) + ", 'b" + std::to_string (i) + "', "
	       + std::to_string (i) + ", " + (i % 3 ? rep ("cd", 80 + i) : std::string ("NULL")) + ")");
    }
  run_sql ("evo2 base", "SELECT * FROM " + t2 + " ORDER BY id");
  direct_dump ("evo2 base", t2);
  run_ddl ("SET SYSTEM PARAMETERS 'alter_table_change_type_strict=no'");
  run_ddl ("ALTER TABLE " + t2 + " MODIFY c VARCHAR(50)");
  run_sql ("evo2 c->varchar", "SELECT * FROM " + t2 + " ORDER BY id");
  direct_dump ("evo2 c->varchar", t2);
  run_ddl ("ALTER TABLE " + t2 + " MODIFY b INT");
  run_sql ("evo2 b->int", "SELECT * FROM " + t2 + " ORDER BY id");
  direct_dump ("evo2 b->int", t2);
  run_ddl ("ALTER TABLE " + t2 + " MODIFY a VARCHAR(8000)");
  run_sql ("evo2 a->char", "SELECT id, a, d FROM " + t2 + " ORDER BY id");
  direct_dump ("evo2 a->char", t2);
  run_ddl ("ALTER TABLE " + t2 + " MODIFY d VARCHAR(9000)");
  run_sql ("evo2 d->bit", "SELECT id, a, d FROM " + t2 + " ORDER BY id");
  direct_dump ("evo2 d->bit", t2);
  run_ddl ("SET SYSTEM PARAMETERS 'alter_table_change_type_strict=yes'");
  drop_table (t2);
}

TEST_F (ReadDiff, SharedAndClassAttributes)
{
  const std::string tn = "t_rd_shared";
  drop_table (tn);
  run_ddl ("CREATE TABLE " + tn +
	   " (id INT, a VARCHAR(5000) STORAGE FORCE_OUTLINE, sh VARCHAR(200) SHARED 'shared value longer than twenty-four bytes', "
	   "b VARCHAR(5000) STORAGE FORCE_OUTLINE, sh2 INT SHARED 7, c VARCHAR(100), d VARCHAR(5000) STORAGE FORCE_OUTLINE)");
  run_ddl ("ALTER TABLE " + tn + " ADD CLASS ATTRIBUTE ca VARCHAR(100) DEFAULT 'class attribute value over twenty four'");
  run_ddl ("ALTER TABLE " + tn + " ADD CLASS ATTRIBUTE cb VARCHAR(5000)");
  for (int i = 0; i < 8; i++)
    {
      const int k = i % 4;
      run_dml ("shared ins", "INSERT INTO " + tn + " (id, a, b, c, d) VALUES (" + std::to_string (i) + ", "
	       + (k == 0 ? std::string ("NULL") : rep ("ab", 100 + 300 * i)) + ", " + (k == 1 ? std::string ("''") : rep ("cd",
		   200 + 100 * i))
	       + ", 'c" + std::to_string (i) + "', " + (k == 2 ? std::string ("NULL") : rep ("ef", 150 + 50 * i)) + ")");
    }
  run_dml ("class attr set", "UPDATE CLASS " + tn + " SET cb = " + rep ("w", 2000));
  run_sql ("shared all", "SELECT * FROM " + tn + " ORDER BY id");
  run_sql ("shared sh,a", "SELECT sh, a FROM " + tn);
  run_sql ("shared a,sh,b", "SELECT a, sh, b, sh2 FROM " + tn);
  run_sql ("shared a,b", "SELECT a, b FROM " + tn);
  run_sql ("shared sh only", "SELECT sh, sh2 FROM " + tn);
  run_sql ("shared pred", "SELECT id, a, d FROM " + tn +
	   " WHERE sh LIKE 'shared%' AND a IS NOT NULL AND d IS NOT NULL ORDER BY id");
  run_sql ("shared pred2", "SELECT id FROM " + tn + " WHERE sh2 = 7 AND b IS NOT NULL ORDER BY id");
  run_sql ("class attr", "SELECT ca, cb FROM CLASS " + tn);
  run_sql ("class attr2", "SELECT ca FROM CLASS " + tn);
  direct_dump ("shared", tn, true);
  run_dml ("shared U shared", "UPDATE " + tn +
	   " SET sh = 'changed shared value that is also longer than 24 bytes', sh2 = 8");
  run_sql ("shared after", "SELECT * FROM " + tn + " ORDER BY id");
  direct_dump ("shared after", tn, true);
  run_ddl ("ALTER TABLE " + tn +
	   " ADD COLUMN x VARCHAR(5000) STORAGE FORCE_OUTLINE DEFAULT 'xdefault-xdefault-xdefault-xdefault'");
  run_sql ("shared after add", "SELECT * FROM " + tn + " ORDER BY id");
  direct_dump ("shared after add", tn, true);
  drop_table (tn);
}

TEST_F (ReadDiff, Indexes)
{
  const std::string tn = "t_rd_idx";
  drop_table (tn);
  run_ddl ("CREATE TABLE " + tn + " (id INT PRIMARY KEY, a INT, b VARCHAR(600) STORAGE FORCE_OUTLINE, c VARCHAR(100), "
	   "d BIT VARYING(16000) STORAGE FORCE_OUTLINE, e NUMERIC(20,4), f VARCHAR(300) STORAGE FORCE_OUTLINE, g INT, p VARCHAR(300) STORAGE FORCE_OUTLINE)");
  run_ddl ("CREATE INDEX i1 ON " + tn + " (a, b, c)");
  run_ddl ("CREATE INDEX i2 ON " + tn + " (c, b, a)");
  run_ddl ("CREATE UNIQUE INDEX i3 ON " + tn + " (b, f)");
  run_ddl ("CREATE INDEX i4 ON " + tn + " (a, d, e, f)");
  run_ddl ("CREATE INDEX i5 ON " + tn + " (p(20))");
  run_ddl ("CREATE INDEX i5b ON " + tn + " (c(2))");
  run_ddl ("CREATE INDEX i6 ON " + tn + " (a DESC, b DESC)");
  run_ddl ("CREATE INDEX i7 ON " + tn + " (c)");
  run_ddl ("CREATE INDEX i8 ON " + tn + " (b)");
  run_ddl ("CREATE INDEX i9 ON " + tn + " (LOWER(b), a)");
  run_ddl ("CREATE INDEX i10 ON " + tn + " (c, b) WHERE b IS NOT NULL");
  run_ddl ("CREATE INDEX i12 ON " + tn + " (e, f, g)");
  for (int r = 0; r < 60; r++)
    {
      const char ch[2] = { (char) ('a' + r % 26), 0 };
      std::string b = r % 9 == 0 ? "NULL" : r % 11 == 0 ? "'bs" + std::to_string (r) + "'" : "REPEAT('" + std::string (
			      ch) + std::to_string (r % 10) + "', " + std::to_string (15 + (r * 13) % 250) + ")";
      std::string c = r % 8 == 0 ? "NULL" : "'c" + std::to_string (r % 5) + "'";
      std::string d = r % 6 == 0 ? "NULL" : "REPEAT(X'" + std::string (r % 2 ? "A5" : "5A") + "', " + std::to_string (
			      25 + r * 3) + ")";
      std::string f = r % 7 == 0 ? "NULL" : "REPEAT('" + std::string (ch) + "', " + std::to_string (26 + (r * 7) % 200) + ")";
      run_dml ("idx ins " + std::to_string (r),
	       "INSERT INTO " + tn + " VALUES (" + std::to_string (r) + ", " + std::to_string (r % 7) + ", " + b + ", " + c + ", " + d
	       + ", " + std::to_string (r) + ".1234" + ", " + f + ", " + std::to_string (r % 3) + ", " +
	       (r % 10 == 0 ? std::string ("NULL") : rep ("pr", 15 + (r * 11) % 120)) + ")");
    }
  run_dml ("idx dup unique", "INSERT INTO " + tn + " SELECT 1000, a, b, c, d, e, f, g, p FROM " + tn + " WHERE id = 3");
  run_dml ("idx dup pk", "INSERT INTO " + tn + " SELECT 3, a, b, c, d, e, f, g, p FROM " + tn + " WHERE id = 4");
  out ("oos_pages=%d\n", 0);
  direct_keys ("idx initial", tn);
  direct_dump ("idx initial", tn);
  direct_variants ("idx initial", tn);
  auto queries = [&] (const std::string &tag)
  {
    run_sql (tag + " all", "SELECT * FROM " + tn + " ORDER BY id");
    run_sql (tag + " i1 cover", "SELECT a, b, c FROM " + tn + " WHERE a >= 2 AND a <= 5 USING INDEX i1");
    run_sql (tag + " i1 cover b", "SELECT a, b, c FROM " + tn + " WHERE a = 3 AND b IS NOT NULL USING INDEX i1");
    run_sql (tag + " i1 noncover", "SELECT id, a, b, c, d FROM " + tn +
	     " WHERE a >= 2 AND a <= 5 USING INDEX i1 ORDER BY id");
    run_sql (tag + " i1 none", "SELECT a, b, c FROM " + tn + " WHERE a >= 2 AND a <= 5 USING INDEX NONE ORDER BY a, b, c");
    run_sql (tag + " i2 cover", "SELECT c, b, a FROM " + tn + " WHERE c = 'c1' USING INDEX i2");
    run_sql (tag + " i2 cover range", "SELECT c, b, a FROM " + tn + " WHERE c >= 'c1' AND c <= 'c3' USING INDEX i2");
    run_sql (tag + " i3 cover", "SELECT b, f FROM " + tn + " WHERE b IS NOT NULL USING INDEX i3");
    run_sql (tag + " i3 range", "SELECT b, f, id FROM " + tn + " WHERE b > 'c' AND b < 'q' USING INDEX i3");
    run_sql (tag + " i4 cover", "SELECT a, d, e, f FROM " + tn + " WHERE a > 0 USING INDEX i4");
    run_sql (tag + " i4 eq", "SELECT id, d FROM " + tn + " WHERE a = 2 AND d IS NOT NULL USING INDEX i4");
    run_sql (tag + " i5 prefix", "SELECT p, id FROM " + tn + " WHERE p > 'a' USING INDEX i5 ORDER BY id");
    run_sql (tag + " i5b prefix", "SELECT c, id FROM " + tn + " WHERE c > 'c1' USING INDEX i5b ORDER BY id");
    run_sql (tag + " i5 like", "SELECT id, p FROM " + tn + " WHERE p LIKE 'a%' USING INDEX i5 ORDER BY id");
    run_sql (tag + " i6 desc", "SELECT a, b FROM " + tn + " WHERE a >= 0 USING INDEX i6 ORDER BY a DESC, b DESC");
    run_sql (tag + " i7", "SELECT c FROM " + tn + " WHERE c IS NOT NULL USING INDEX i7");
    run_sql (tag + " i8", "SELECT b FROM " + tn + " WHERE b IS NOT NULL USING INDEX i8");
    run_sql (tag + " i8 eq", "SELECT id, b FROM " + tn + " WHERE b = REPEAT('c3', 28) USING INDEX i8");
    run_sql (tag + " i9 func", "SELECT id, LOWER(b), a FROM " + tn +
	     " WHERE LOWER(b) > 'a' AND a > 1 USING INDEX i9 ORDER BY id");
    run_sql (tag + " i10 filter", "SELECT c, b FROM " + tn + " WHERE b IS NOT NULL AND c > 'c0' USING INDEX i10");
    run_sql (tag + " i12 cover", "SELECT e, f, g FROM " + tn + " WHERE e > 5 USING INDEX i12");
    run_sql (tag + " i12 skip", "SELECT e, f, g FROM " + tn + " WHERE e > 5 USING INDEX i12 ORDER BY e, f, g");
    run_sql (tag + " count", "SELECT COUNT(*) FROM " + tn + " WHERE b IS NOT NULL AND f IS NOT NULL");
    run_sql (tag + " min/max", "SELECT MIN(b), MAX(b), MIN(f), MAX(f) FROM " + tn);
    run_sql (tag + " group", "SELECT a, COUNT(*) FROM " + tn + " GROUP BY a USING INDEX i1 ORDER BY a");
    run_sql (tag + " join idx", "SELECT x.id, y.id FROM " + tn + " x, " + tn +
	     " y WHERE x.b = y.b AND x.id < y.id ORDER BY 1, 2");
    run_sql (tag + " forupdate", "SELECT id, b, f FROM " + tn + " WHERE a = 2 FOR UPDATE");
    db_commit_transaction ();
  };
  queries ("idx Q1");
  run_dml ("idx U b", "UPDATE " + tn + " SET b = CONCAT('z9-', CAST(id AS VARCHAR), '-', " + rep ("zq",
	   60) + ") WHERE id % 4 = 1");
  run_dml ("idx U b->short", "UPDATE " + tn + " SET b = 'tiny' WHERE id % 7 = 2");
  run_dml ("idx U f", "UPDATE " + tn + " SET f = CONCAT(f, '-x') WHERE id % 5 = 3 AND f IS NOT NULL");
  run_dml ("idx U d", "UPDATE " + tn + " SET d = REPEAT(X'77', 900) WHERE id % 6 = 4");
  run_dml ("idx U a", "UPDATE " + tn + " SET a = a + 1 WHERE id % 3 = 0");
  run_dml ("idx U unique violate", "UPDATE " + tn + " SET b = (SELECT b FROM " + tn +
	   " WHERE id = 1), f = (SELECT f FROM " + tn + " WHERE id = 1) WHERE id = 2");
  run_dml ("idx D", "DELETE FROM " + tn + " WHERE id % 10 = 5");
  direct_keys ("idx after DML", tn);
  queries ("idx Q2");
  run_ddl ("CREATE INDEX i13 ON " + tn + " (g, b, f, d)");
  run_ddl ("CREATE INDEX i14 ON " + tn + " (f, c)");
  run_sql ("idx Q3 i13", "SELECT g, b, f, d FROM " + tn + " WHERE g >= 0 USING INDEX i13");
  run_sql ("idx Q3 i14", "SELECT f, c FROM " + tn + " WHERE f IS NOT NULL USING INDEX i14");
  direct_keys ("idx after create index", tn);
  direct_dump ("idx after create index", tn);
  run_dml ("idx insert after", "INSERT INTO " + tn + " VALUES (500, 1, " + rep ("n",
	   550) + ", 'cn', REPEAT(X'01', 200), 1.5, " + rep ("o", 250) + ", 2, " + rep ("pz", 60) + ")");
  run_sql ("idx Q4 new", "SELECT id, b, f FROM " + tn + " WHERE id >= 500 USING INDEX i3");
  run_ddl ("ALTER TABLE " + tn + " ADD COLUMN h VARCHAR(100) DEFAULT 'dflt'");
  run_ddl ("CREATE INDEX i15 ON " + tn + " (h, b)");
  run_sql ("idx Q5 i15", "SELECT h, b FROM " + tn + " WHERE h = 'dflt' USING INDEX i15");
  direct_keys ("idx after add col", tn);
  drop_table (tn);
}

TEST_F (ReadDiff, Partitioned)
{
  const std::string tn = "t_rd_part";
  drop_table (tn);
  run_ddl ("CREATE TABLE " + tn +
	   " (id INT, a VARCHAR(5000) STORAGE FORCE_OUTLINE, b VARCHAR(100), c VARCHAR(5000) STORAGE FORCE_OUTLINE) "
	   "PARTITION BY HASH (id) PARTITIONS 3");
  for (int i = 0; i < 30; i++)
    {
      const int k = i % 4;
      run_dml ("part ins", "INSERT INTO " + tn + " VALUES (" + std::to_string (i) + ", " +
	       (k == 0 ? std::string ("NULL") : rep ("ab", 40 + 70 * i)) + ", 'b"
	       + std::to_string (i) + "', " + (k == 1 ? std::string ("''") : rep ("cd", 30 + 60 * i)) + ")");
    }
  run_sql ("part all", "SELECT * FROM " + tn + " ORDER BY id");
  run_sql ("part a,c", "SELECT a, c FROM " + tn + " WHERE id > 5");
  run_sql ("part a", "SELECT a FROM " + tn);
  run_sql ("part pred", "SELECT id FROM " + tn + " WHERE a IS NOT NULL AND c IS NOT NULL AND id > 3 ORDER BY id");
  run_sql ("part p0", "SELECT id, a, c FROM " + tn + "__p__p0 ORDER BY id");
  run_dml ("part move", "UPDATE " + tn + " SET id = id + 1 WHERE id % 2 = 0");
  run_sql ("part after move", "SELECT * FROM " + tn + " ORDER BY id");
  run_dml ("part move2", "UPDATE " + tn + " SET id = id + 100, a = c WHERE id % 3 = 0");
  run_sql ("part after move2", "SELECT * FROM " + tn + " ORDER BY id");
  run_ddl ("ALTER TABLE " + tn +
	   " ADD COLUMN d VARCHAR(5000) STORAGE FORCE_OUTLINE DEFAULT 'partition default value over 24 bytes'");
  run_sql ("part after add", "SELECT * FROM " + tn + " ORDER BY id");
  run_sql ("part after add d", "SELECT id, d, a FROM " + tn + " ORDER BY id");
  for (int p = 0; p < 3; p++)
    {
      const std::string pn = tn + "__p__p" + std::to_string (p);
      run_sql ("part p" + std::to_string (p), "SELECT * FROM " + pn + " ORDER BY id");
      direct_dump ("part p" + std::to_string (p), pn);
    }
  drop_table (tn);
}

TEST_F (ReadDiff, Workspace)
{
  const std::string tn = "t_rd_ws";
  drop_table (tn);
  run_ddl ("CREATE TABLE " + tn +
	   " (id INT, v1 VARCHAR(5000) STORAGE FORCE_OUTLINE, n INT, v2 VARCHAR(5000) STORAGE FORCE_OUTLINE, "
	   "s VARCHAR(100), b BIT VARYING(100000) STORAGE FORCE_OUTLINE)");
  for (int i = 0; i < 8; i++)
    {
      run_dml ("ws ins", "INSERT INTO " + tn + " VALUES (" + std::to_string (i) + ", " + (i % 3 == 0 ? std::string ("NULL") :
	       rep ("ab", 50 + 50 * i)) + ", " + std::to_string (i)
	       + ", " + (i % 4 == 1 ? std::string ("''") : rep ("cd",
			 60 + 40 * i)) + ", 's" + std::to_string (i) + "', REPEAT(X'AB', " + std::to_string (30 + 100 * i) + "))");
    }
  auto ws_dump = [&] (const std::string &tag)
  {
    out ("\n#### WS %s\n", tag.c_str ());
    DB_OBJECT *cls = db_find_class (tn.c_str ());
    if (cls == nullptr)
      {
	out ("no class\n");
	return;
      }
    DB_OBJLIST *objects = db_fetch_all_objects (cls, DB_FETCH_READ);
    std::map<int, std::string> lines;
    for (DB_OBJLIST *item = objects; item != nullptr; item = item->next)
      {
	std::string line;
	int id = -1;
	for (const char *attr :
	     { "id", "v1", "n", "v2", "s", "b"
	     })
	  {
	    DB_VALUE v;
	    db_make_null (&v);
	    const int e = db_get (item->op, attr, &v);
	    if (e != NO_ERROR)
	      {
		line += " <err " + std::to_string (e) + ">";
		continue;
	      }
	    if (std::string (attr) == "id" && !db_value_is_null (&v))
	      {
		id = db_get_int (&v);
	      }
	    line += " " + dump_value (&v);
	    db_value_clear (&v);
	  }
	lines[id * 1000 + (int) lines.size ()] = line;
      }
    for (auto &kv : lines)
      {
	out ("W%s\n", kv.second.c_str ());
      }
    if (objects != nullptr)
      {
	db_objlist_free (objects);
      }
  };
  ws_dump ("initial");
  // workspace update (copy-area path)
  {
    DB_OBJECT *cls = db_find_class (tn.c_str ());
    DB_OBJLIST *objects = db_fetch_all_objects (cls, DB_FETCH_WRITE);
    int k = 0;
    for (DB_OBJLIST *item = objects; item != nullptr; item = item->next, k++)
      {
	DB_VALUE v;
	if (k % 2 == 0)
	  {
	    db_make_int (&v, 1000 + k);
	    db_put (item->op, "n", &v);
	  }
	else
	  {
	    std::string big (3000 + k * 100, (char) ('a' + k));
	    db_make_string (&v, big.c_str ());
	    db_put (item->op, "v1", &v);
	    db_value_clear (&v);
	  }
      }
    if (objects != nullptr)
      {
	db_objlist_free (objects);
      }
    out ("ws commit=%d\n", db_commit_transaction ());
  }
  run_sql ("ws after put", "SELECT * FROM " + tn + " ORDER BY id");
  ws_dump ("after put");
  direct_dump ("ws after put", tn);
  drop_table (tn);
}

TEST_F (ReadDiff, LazyPredicateManyRows)
{
  const std::string tn = "t_rd_lazy";
  drop_table (tn);
  run_ddl ("CREATE TABLE " + tn +
	   " (id INT, k INT, s1 VARCHAR(300) STORAGE FORCE_OUTLINE, s2 VARCHAR(300) STORAGE FORCE_OUTLINE, p VARCHAR(50))");
  for (int batch = 0; batch < 12; batch++)
    {
      std::string sql = "INSERT INTO " + tn + " VALUES ";
      for (int i = 0; i < 1000; i++)
	{
	  const int id = batch * 1000 + i;
	  if (i)
	    {
	      sql += ",";
	    }
	  sql += "(" + std::to_string (id) + "," + std::to_string (id % 17) + ","
		 + (id % 5 == 0 ? std::string ("NULL") : "'" + std::string (30 + id % 100, (char) ('a' + id % 26)) + "'") + ","
		 + (id % 7 == 0 ? std::string ("NULL") : "'" + std::string (26 + id % 50,
		     (char) ('A' + id % 26)) + "'") + ", 'p" + std::to_string (id % 10) + "')";
	}
      run_dml ("lazy batch " + std::to_string (batch), sql);
    }
  out ("oos_pages=%d\n", 0);
  run_sql ("lazy count1", "SELECT COUNT(*) FROM " + tn + " WHERE k = 3 AND s1 IS NOT NULL AND s2 IS NOT NULL");
  run_sql ("lazy count2", "SELECT COUNT(*), SUM(LENGTH(s1)), SUM(LENGTH(s2)) FROM " + tn +
	   " WHERE k > 8 AND s1 IS NOT NULL");
  run_sql ("lazy count3", "SELECT COUNT(*) FROM " + tn + " WHERE p = 'p3' AND k < 5 AND LENGTH(s2) > 30");
  run_sql ("lazy count4", "SELECT COUNT(*) FROM " + tn + " WHERE k = 1 AND s1 > s2");
  run_sql ("lazy agg", "SELECT k, COUNT(*), MAX(s1), MIN(s2) FROM " + tn + " GROUP BY k ORDER BY k");
  run_sql ("lazy rows", "SELECT id, s1, s2 FROM " + tn + " WHERE k = 5 AND id > 11000 ORDER BY id");
  drop_table (tn);
}

TEST_F (ReadDiff, ColumnCounts)
{
  const int counts[] = { 1, 2, 3, 4, 7, 8, 9, 10, 16 };
  for (int n : counts)
    {
      const std::string tn = "t_rd_n" + std::to_string (n);
      drop_table (tn);
      std::string ddl = "CREATE TABLE " + tn + " (id INT";
      for (int k = 1; k < n; k++)
	{
	  ddl += ", c" + std::to_string (k) + " ";
	  ddl += k % 4 == 1 ? "VARCHAR(5000) STORAGE FORCE_OUTLINE" : k % 4 == 2 ? "INT" : k % 4 == 3 ? "DOUBLE" : "VARCHAR(100)";
	}
      run_ddl (ddl + ")");
      uint64_t x = 12345 + n;
      auto nxt = [&] ()
      {
	x = x * 6364136223846793005ULL + 1442695040888963407ULL;
	return (int) (x >> 40);
      };
      for (int r = 0; r < 14; r++)
	{
	  std::string sql = "INSERT INTO " + tn + " VALUES (" + std::to_string (r);
	  for (int k = 1; k < n; k++)
	    {
	      const int roll = nxt () % 6;
	      std::string v;
	      if (k % 4 == 1)
		{
		  static const int L[6] = { -1, 0, 10, 30, 200, 3000 };
		  const int len = L[roll];
		  v = len < 0 ? "NULL" : len == 0 ? "''" : rep ("ab", (len + 1) / 2 + r);
		}
	      else if (k % 4 == 2)
		{
		  v = roll == 0 ? "NULL" : std::to_string (nxt () % 1000);
		}
	      else if (k % 4 == 3)
		{
		  v = roll == 1 ? "NULL" : std::to_string (nxt () % 1000) + ".5";
		}
	      else
		{
		  v = roll == 2 ? "NULL" : roll == 3 ? "''" : "'v" + std::to_string (nxt () % 100000) + "'";
		}
	      sql += ", " + v;
	    }
	  run_dml ("ncol ins", sql + ")");
	}
      run_sql ("ncol " + std::to_string (n) + " all", "SELECT * FROM " + tn + " ORDER BY id");
      direct_dump ("ncol " + std::to_string (n), tn);
      run_dml ("ncol upd", "UPDATE " + tn + " SET id = id + 0 WHERE id % 2 = 0");
      run_sql ("ncol " + std::to_string (n) + " after upd", "SELECT * FROM " + tn + " ORDER BY id");
      drop_table (tn);
    }
}

TEST_F (ReadDiff, OffsetSweep)
{
  const std::string tn = "t_rd_off";
  drop_table (tn);
  run_ddl ("CREATE TABLE " + tn +
	   " (id INT, p VARCHAR(400), q VARCHAR(5000) STORAGE FORCE_OUTLINE, r VARCHAR(400), z INT)");
  for (int k = 0; k < 131; k++)
    {
      run_dml ("off ins", "INSERT INTO " + tn + " VALUES (" + std::to_string (k) + ", " + rep ("pq",
	       (180 + k) / 2 + (k % 2)) + ", " + rep ("qr", (40 + (k % 7) * 50) / 2) + ", "
	       + (k % 30 == 0 ? std::string ("NULL") : rep ("r",
		   k % 30)) + ", " + (k % 9 == 0 ? std::string ("NULL") : std::to_string (k)) + ")");
    }
  run_sql ("off all", "SELECT * FROM " + tn + " ORDER BY id");
  run_sql ("off p", "SELECT p FROM " + tn);
  run_sql ("off q,r", "SELECT q, r FROM " + tn);
  run_sql ("off pred", "SELECT id FROM " + tn + " WHERE z IS NULL AND q IS NOT NULL AND r IS NOT NULL ORDER BY id");
  direct_dump ("off", tn);
  run_dml ("off upd", "UPDATE " + tn + " SET p = CONCAT(p, 'xx') WHERE id % 3 = 0");
  run_sql ("off after upd", "SELECT * FROM " + tn + " ORDER BY id");
  direct_dump ("off after upd", tn);
  drop_table (tn);
}

TEST_F (ReadDiff, BigFixed)
{
  for (int bits :
       {
	       4000, 80000, 200000, 400000
       })
    {
      const std::string tn = "t_rd_fix" + std::to_string (bits);
      drop_table (tn);
      run_ddl ("CREATE TABLE " + tn + " (id INT, a VARCHAR(5000) STORAGE FORCE_OUTLINE, big BIT(" + std::to_string (bits)
	       + "), b VARCHAR(100), c VARCHAR(5000) STORAGE FORCE_OUTLINE, n INT)");
      for (int r = 0; r < 6; r++)
	{
	  run_dml ("fix ins", "INSERT INTO " + tn + " VALUES (" + std::to_string (r) + ", " +
		   (r % 3 == 0 ? std::string ("NULL") : rep ("ab", 100 + r * 50)) + ", "
		   + (r == 2 ? std::string ("NULL") : "CAST(REPEAT(X'" + std::string (r % 2 ? "A5" : "5A") + "', " + std::to_string (
			      bits / 8) + ") AS BIT(" + std::to_string (bits) + "))")
		   + ", 'b" + std::to_string (r) + "', " + (r % 4 == 1 ? std::string ("''") : rep ("cd",
		       30 + r * 100)) + ", " + std::to_string (r) + ")");
	}
      run_sql ("fix " + std::to_string (bits) + " all", "SELECT id, a, b, c, n FROM " + tn + " ORDER BY id");
      run_sql ("fix " + std::to_string (bits) + " big", "SELECT id, big FROM " + tn + " ORDER BY id");
      run_sql ("fix " + std::to_string (bits) + " bigall", "SELECT * FROM " + tn + " ORDER BY id");
      run_sql ("fix " + std::to_string (bits) + " pred",
	       "SELECT id, a FROM " + tn + " WHERE n >= 0 AND a IS NOT NULL AND c IS NOT NULL ORDER BY id");
      direct_dump ("fix " + std::to_string (bits), tn);
      run_dml ("fix upd", "UPDATE " + tn + " SET n = n + 1, b = 'upd' WHERE id % 2 = 0");
      run_sql ("fix " + std::to_string (bits) + " after", "SELECT id, a, b, c, n FROM " + tn + " ORDER BY id");
      direct_dump ("fix after " + std::to_string (bits), tn);
      drop_table (tn);
    }
}

TEST_F (ReadDiff, UncommittedAndCopies)
{
  const std::string tn = "t_rd_unc";
  drop_table (tn);
  run_ddl ("CREATE TABLE " + tn +
	   " (id INT PRIMARY KEY, a VARCHAR(5000) STORAGE FORCE_OUTLINE, b VARCHAR(5000) STORAGE FORCE_OUTLINE, c VARCHAR(100), d VARCHAR(5000) STORAGE FORCE_OUTLINE)");
  run_ddl ("CREATE INDEX unc_i1 ON " + tn + " (c, a)");
  run_ddl ("CREATE INDEX unc_i2 ON " + tn + " (a, b, c)");
  // statements inside one open transaction, no commits
  for (int i = 0; i < 6; i++)
    {
      const int rc = exec_sql (("INSERT INTO " + tn + " VALUES (" + std::to_string (i) + ", " + rep ("ab",
				40 + 60 * i) + ", " + (i % 2 ? std::string ("NULL") : rep ("cd", 100 + 30 * i))
				+ ", 'c" + std::to_string (i % 3) + "', " + rep ("ef", 70 + 20 * i) + ")").c_str ());
      out ("open insert rc=%d\n", rc);
    }
  run_sql ("unc all", "SELECT * FROM " + tn + " ORDER BY id");
  run_sql ("unc a,b", "SELECT a, b FROM " + tn);
  run_sql ("unc idx", "SELECT c, a FROM " + tn + " WHERE c >= 'c0' USING INDEX unc_i1");
  run_sql ("unc idx2", "SELECT a, b, c FROM " + tn + " WHERE a > 'a' USING INDEX unc_i2");
  direct_dump ("unc open inserts", tn);
  direct_keys ("unc open inserts", tn);
  out ("open update rc=%d\n", exec_sql (("UPDATE " + tn + " SET a = " + rep ("zz",
					 300) + ", b = a WHERE id % 2 = 0").c_str ()));
  out ("open update2 rc=%d\n", exec_sql (("UPDATE " + tn +
					  " SET a = CONCAT(a, 'tail'), d = NULL WHERE id < 4").c_str ()));
  run_sql ("unc after upd", "SELECT * FROM " + tn + " ORDER BY id");
  run_sql ("unc idx after upd", "SELECT a, b, c FROM " + tn + " WHERE a > 'a' USING INDEX unc_i2");
  direct_dump ("unc open updates", tn);
  direct_keys ("unc open updates", tn);
  out ("open delete rc=%d\n", exec_sql (("DELETE FROM " + tn + " WHERE id = 3").c_str ()));
  run_sql ("unc after del", "SELECT * FROM " + tn + " ORDER BY id");
  db_abort_transaction ();
  run_sql ("unc after rollback", "SELECT * FROM " + tn + " ORDER BY id");
  for (int i = 0; i < 6; i++)
    {
      run_dml ("unc ins2", "INSERT INTO " + tn + " VALUES (" + std::to_string (i) + ", " + rep ("ab",
	       40 + 60 * i) + ", " + (i % 2 ? std::string ("NULL") : rep ("cd", 100 + 30 * i))
	       + ", 'c" + std::to_string (i % 3) + "', " + rep ("ef", 70 + 20 * i) + ")");
    }
  // copies
  run_ddl ("CREATE TABLE t_rd_unc_copy AS SELECT * FROM " + tn);
  run_sql ("copy ctas", "SELECT * FROM t_rd_unc_copy ORDER BY id");
  direct_dump ("copy ctas", "t_rd_unc_copy");
  run_ddl ("CREATE TABLE t_rd_unc_copy2 LIKE " + tn);
  run_dml ("copy insert select", "INSERT INTO t_rd_unc_copy2 SELECT * FROM " + tn);
  run_dml ("copy insert select2", "INSERT INTO t_rd_unc_copy2 SELECT id + 100, b, a, c, d FROM " + tn);
  run_sql ("copy 2", "SELECT * FROM t_rd_unc_copy2 ORDER BY id");
  direct_dump ("copy 2", "t_rd_unc_copy2");
  direct_keys ("copy 2", "t_rd_unc_copy2");
  run_dml ("copy update join", "UPDATE t_rd_unc_copy2 x, " + tn + " y SET x.d = y.a WHERE x.id = y.id");
  run_sql ("copy 2 after join upd", "SELECT * FROM t_rd_unc_copy2 ORDER BY id");
  run_dml ("copy delete join", "DELETE FROM t_rd_unc_copy2 WHERE id IN (SELECT id FROM " + tn + " WHERE a > 'm')");
  run_sql ("copy 2 after del", "SELECT * FROM t_rd_unc_copy2 ORDER BY id");
  run_sql ("copy union", "SELECT a, b FROM " + tn + " UNION SELECT b, a FROM t_rd_unc_copy2 ORDER BY 1, 2");
  drop_table ("t_rd_unc_copy");
  drop_table ("t_rd_unc_copy2");
  drop_table (tn);
}

TEST_F (ReadDiff, ManyVariableColumns)
{
  for (int n :
       {
	       100, 300
       })
    {
      const std::string tn = "t_rd_many" + std::to_string (n);
      drop_table (tn);
      std::string ddl = "CREATE TABLE " + tn + " (id INT";
      for (int k = 1; k <= n; k++)
	{
	  ddl += ", v" + std::to_string (k) + " VARCHAR(2000)" + (k % 25 == 0 ? " STORAGE FORCE_OUTLINE" : "");
	}
      run_ddl (ddl + ", z INT)");
      for (int r = 0; r < 8; r++)
	{
	  std::string sql = "INSERT INTO " + tn + " VALUES (" + std::to_string (r);
	  for (int k = 1; k <= n; k++)
	    {
	      const int roll = (r * 7 + k * 3) % 11;
	      std::string v = roll < 5 ? "NULL" : roll == 5 ? "''" : roll < 9 ? "'v" + std::to_string (r) + "_" + std::to_string (
				      k) + "'" : rep ("m", 30 + (k * 5) % 100);
	      if (k % 25 == 0 && r % 2 == 0)
		{
		  v = rep ("o", 40 + r * 20);
		}
	      sql += ", " + v;
	    }
	  run_dml ("many ins", sql + ", " + std::to_string (r) + ")");
	}
      run_sql ("many " + std::to_string (n) + " all", "SELECT * FROM " + tn + " ORDER BY id");
      run_sql ("many " + std::to_string (n) + " last", "SELECT v" + std::to_string (n) + ", z FROM " + tn);
      run_sql ("many " + std::to_string (n) + " first/last", "SELECT v1, v" + std::to_string (n) + " FROM " + tn);
      run_sql ("many " + std::to_string (n) + " oos", "SELECT v25, v50, v75, v100 FROM " + tn);
      run_sql ("many " + std::to_string (n) + " pred",
	       "SELECT id FROM " + tn + " WHERE v25 IS NOT NULL AND v50 IS NOT NULL AND z > 2 ORDER BY id");
      direct_dump ("many " + std::to_string (n), tn);
      direct_variants ("many " + std::to_string (n), tn);
      run_dml ("many upd", "UPDATE " + tn + " SET v1 = 'u', v25 = NULL, z = z + 1 WHERE id % 2 = 1");
      run_sql ("many " + std::to_string (n) + " after upd", "SELECT * FROM " + tn + " ORDER BY id");
      drop_table (tn);
    }
  // only variable columns, boundary rows (all NULL, trailing empty, single OOS)
  const std::string t2 = "t_rd_onlyvar";
  drop_table (t2);
  run_ddl ("CREATE TABLE " + t2 + " (a VARCHAR(100), b VARCHAR(5000) STORAGE FORCE_OUTLINE, c VARCHAR(100))");
  const char *vals[][3] =
  {
    { "NULL", "NULL", "NULL" }, { "''", "NULL", "NULL" }, { "NULL", "NULL", "'x'" }, { "NULL", "''", "NULL" },
    { "'a'", "'b'", "'c'" }, { "''", "''", "''" }, { "NULL", "NULL", "''" },
  };
  for (auto &v : vals)
    {
      run_dml ("onlyvar ins", std::string ("INSERT INTO ") + t2 + " VALUES (" + v[0] + ", " + v[1] + ", " + v[2] + ")");
    }
  for (int i = 0; i < 4; i++)
    {
      run_dml ("onlyvar ins oos", "INSERT INTO " + t2 + " VALUES (" + (i % 2 ? "NULL" : "'q'") + ", " + rep ("ab",
	       40 + i * 100) + ", " + (i > 1 ? "NULL" : "'t'") + ")");
    }
  run_sql ("onlyvar all", "SELECT * FROM " + t2);
  run_sql ("onlyvar b", "SELECT b FROM " + t2);
  run_sql ("onlyvar c", "SELECT c FROM " + t2);
  run_sql ("onlyvar a,c", "SELECT a, c FROM " + t2);
  run_sql ("onlyvar pred", "SELECT a, b FROM " + t2 + " WHERE c IS NULL AND b IS NOT NULL");
  direct_dump ("onlyvar", t2);
  drop_table (t2);
}

TEST_F (ReadDiff, InheritanceAndConstraints)
{
  drop_table ("t_rd_child");
  drop_table ("t_rd_child2");
  drop_table ("t_rd_par");
  run_ddl ("CREATE TABLE t_rd_par (id INT, a VARCHAR(5000) STORAGE FORCE_OUTLINE, b VARCHAR(100), pk INT)");
  run_ddl ("CREATE TABLE t_rd_child UNDER t_rd_par (c VARCHAR(5000) STORAGE FORCE_OUTLINE, d INT, e VARCHAR(5000) STORAGE FORCE_OUTLINE)");
  run_ddl ("CREATE TABLE t_rd_child2 UNDER t_rd_par (x INT, y VARCHAR(5000) STORAGE FORCE_OUTLINE)");
  for (int i = 0; i < 6; i++)
    {
      run_dml ("inh par", "INSERT INTO t_rd_par VALUES (" + std::to_string (i) + ", " + rep ("pa",
	       40 + 50 * i) + ", 'b" + std::to_string (i) + "', " + std::to_string (i) + ")");
      run_dml ("inh child", "INSERT INTO t_rd_child VALUES (" + std::to_string (100 + i) + ", " +
	       (i % 3 == 0 ? std::string ("NULL") : rep ("ca", 40 + 50 * i)) + ", 'cb" + std::to_string (i) + "', "
	       + std::to_string (i) + ", " + rep ("cc",
		   30 + 70 * i) + ", " + std::to_string (i * 2) + ", " + (i % 2 ? std::string ("NULL") : rep ("ce", 60 + 40 * i)) + ")");
      run_dml ("inh child2", "INSERT INTO t_rd_child2 VALUES (" + std::to_string (200 + i) + ", " + rep ("xa",
	       40 + 50 * i) + ", 'xb" + std::to_string (i) + "', " + std::to_string (i) + ", "
	       + std::to_string (i) + ", " + rep ("xy", 20 + 90 * i) + ")");
    }
  run_sql ("inh par all", "SELECT * FROM t_rd_par ORDER BY id");
  run_sql ("inh par a", "SELECT a FROM t_rd_par");
  run_sql ("inh par id,a,b", "SELECT id, a, b FROM t_rd_par ORDER BY id");
  run_sql ("inh all par", "SELECT id, a, b FROM ALL t_rd_par ORDER BY id");
  run_sql ("inh all par pred", "SELECT id, a FROM ALL t_rd_par WHERE pk > 1 AND a IS NOT NULL ORDER BY id");
  run_sql ("inh child all", "SELECT * FROM t_rd_child ORDER BY id");
  run_sql ("inh child ce", "SELECT c, e FROM t_rd_child");
  run_sql ("inh child par attrs", "SELECT a, c, e, b FROM t_rd_child ORDER BY id");
  run_dml ("inh upd par", "UPDATE ALL t_rd_par SET b = 'updated', pk = pk + 1");
  run_sql ("inh after upd", "SELECT id, a, b, pk FROM ALL t_rd_par ORDER BY id");
  direct_dump ("inh par", "t_rd_par");
  direct_dump ("inh child", "t_rd_child");
  direct_dump ("inh child2", "t_rd_child2");
  run_ddl ("ALTER TABLE t_rd_par ADD COLUMN pz VARCHAR(5000) STORAGE FORCE_OUTLINE DEFAULT 'parent added column default that is long enough'");
  run_sql ("inh after add", "SELECT * FROM t_rd_child ORDER BY id");
  run_sql ("inh after add all", "SELECT id, pz, a FROM ALL t_rd_par ORDER BY id");
  direct_dump ("inh child after add", "t_rd_child");
  drop_table ("t_rd_child");
  drop_table ("t_rd_child2");
  drop_table ("t_rd_par");

  // constraints with OOS key columns
  drop_table ("t_rd_fkc");
  drop_table ("t_rd_fkp");
  run_ddl ("CREATE TABLE t_rd_fkp (a INT, k VARCHAR(300) STORAGE FORCE_OUTLINE, pay VARCHAR(5000) STORAGE FORCE_OUTLINE, PRIMARY KEY (a, k))");
  run_ddl ("CREATE TABLE t_rd_fkc (id INT PRIMARY KEY, a INT, k VARCHAR(300) STORAGE FORCE_OUTLINE, note VARCHAR(5000) STORAGE FORCE_OUTLINE, "
	   "FOREIGN KEY (a, k) REFERENCES t_rd_fkp (a, k) ON DELETE RESTRICT ON UPDATE RESTRICT)");
  for (int i = 0; i < 8; i++)
    {
      run_dml ("fk par", "INSERT INTO t_rd_fkp VALUES (" + std::to_string (i % 3) + ", " + rep ("kk",
	       20 + i * 7) + ", " + rep ("pp", 50 + i * 40) + ")");
    }
  for (int i = 0; i < 8; i++)
    {
      run_dml ("fk child ok", "INSERT INTO t_rd_fkc VALUES (" + std::to_string (i) + ", " + std::to_string (
		       i % 3) + ", " + rep ("kk", 20 + i * 7) + ", " + rep ("nn", 70 + i * 30) + ")");
    }
  run_dml ("fk child bad", "INSERT INTO t_rd_fkc VALUES (50, 1, " + rep ("kk", 3) + ", 'x')");
  run_dml ("fk child null", "INSERT INTO t_rd_fkc VALUES (51, NULL, " + rep ("kk", 20) + ", 'x')");
  run_dml ("fk parent del restricted", "DELETE FROM t_rd_fkp WHERE a = 0");
  run_dml ("fk parent upd restricted", "UPDATE t_rd_fkp SET k = " + rep ("zz", 30) + " WHERE a = 1");
  run_sql ("fk par all", "SELECT * FROM t_rd_fkp ORDER BY a, k");
  run_sql ("fk child all", "SELECT * FROM t_rd_fkc ORDER BY id");
  run_sql ("fk join", "SELECT c.id, p.pay FROM t_rd_fkc c, t_rd_fkp p WHERE c.a = p.a AND c.k = p.k ORDER BY c.id");
  direct_keys ("fk par", "t_rd_fkp");
  direct_keys ("fk child", "t_rd_fkc");
  run_dml ("fk child del", "DELETE FROM t_rd_fkc WHERE a = 0");
  run_dml ("fk parent del ok", "DELETE FROM t_rd_fkp WHERE a = 0");
  run_sql ("fk par after", "SELECT * FROM t_rd_fkp ORDER BY a, k");
  drop_table ("t_rd_fkc");
  drop_table ("t_rd_fkp");

  // trigger reading OOS values
  drop_table ("t_rd_trg_log");
  drop_table ("t_rd_trg");
  run_ddl ("CREATE TABLE t_rd_trg (id INT PRIMARY KEY, a VARCHAR(5000) STORAGE FORCE_OUTLINE, b VARCHAR(5000) STORAGE FORCE_OUTLINE)");
  run_ddl ("CREATE TABLE t_rd_trg_log (id INT, olda VARCHAR(5000) STORAGE FORCE_OUTLINE, newa VARCHAR(5000) STORAGE FORCE_OUTLINE)");
  run_ddl ("CREATE TRIGGER t_rd_trg1 AFTER UPDATE ON t_rd_trg EXECUTE INSERT INTO t_rd_trg_log VALUES (obj.id, obj.a, obj.b)");
  for (int i = 0; i < 4; i++)
    {
      run_dml ("trg ins", "INSERT INTO t_rd_trg VALUES (" + std::to_string (i) + ", " + rep ("ta",
	       40 + 60 * i) + ", " + rep ("tb", 50 + 30 * i) + ")");
    }
  run_dml ("trg upd", "UPDATE t_rd_trg SET a = " + rep ("tz", 200) + " WHERE id % 2 = 0");
  run_dml ("trg upd2", "UPDATE t_rd_trg SET b = a WHERE id < 3");
  run_sql ("trg all", "SELECT * FROM t_rd_trg ORDER BY id");
  run_sql ("trg log", "SELECT * FROM t_rd_trg_log ORDER BY id, olda");
  run_ddl ("DROP TRIGGER t_rd_trg1");
  drop_table ("t_rd_trg_log");
  drop_table ("t_rd_trg");
}

int
main (int argc, char **argv)
{
  ::testing::InitGoogleTest (&argc, argv);
  ::testing::AddGlobalTestEnvironment (new SqlServerEnv ());
  return RUN_ALL_TESTS ();
}
