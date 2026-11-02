/*
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * test_oos_sql_zz_writediff.cpp - adversarial review repro (PR 8299, adversary 4).
 *
 * Runs a fixed script of INSERT / UPDATE statements (server-side DML, client workspace flush,
 * trigger-driven client-side multi-update, partition moves, click counters, serials) and dumps the
 * raw heap record bytes of every visible row, with run-dependent fields masked. The same file is
 * built against the PR head and its merge base; the two dumps are compared with diff.
 *
 * Masked fields (replaced by 0xEE / 0x00 in the masked hash and hex):
 *   - db_serial created_time / updated_time DATETIME values (wall clock of CREATE SERIAL)
 *   - MVCC header bytes after the CHN (insert id, delete id, previous-version LSA)
 *   - OOS stub OID (bytes 0..7 of the stub) and identity stamp (bytes 16..23 of the stub)
 *   - VOT alignment padding bytes after the last VOT entry (never written by the writer)
 *   - bytes in the fixed area that no fixed attribute covers (never written by the writer)
 *   - bound-bit bits beyond the number of fixed attributes (never written by the writer)
 * Page ids of OIDs are not printed; slot ids are printed. OOS payload bytes are read through the
 * stub and hashed (FNV-1a 64), so OOS content is still compared.
 *
 * The original values of the masked padding bytes and bits are hashed on a separate
 * "padding_bytes=... padding_fnv=..." line. The writer never initializes them, so that line changes
 * from run to run of the same build; compare dumps with that line removed:
 *   diff <(grep -v padding_bytes= base_dump.txt) <(grep -v padding_bytes= head_dump.txt)
 *
 * Output path: $WRITEDIFF_OUT, else ./writediff_dump.txt in the test working directory.
 */

#include "test_oos_sql_common.hpp"

#include "heap_oos.hpp"
#include "heap_pending_record.hpp"
#include "record_descriptor.hpp"
#include "language_support.h"
#include "locator_cl.h"
#include "log_impl.h"
#include "object_domain.h"
#include "object_primitive.h"
#include "object_representation.h"
#include "object_representation_sr.h"
#include "scope_exit.hpp"
#include "server_interface.h"
#include "work_space.h"
#include "xserver_interface.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// XXX: SHOULD BE THE LAST INCLUDE HEADER
#include "memory_wrapper.hpp"

namespace
{
  FILE *g_out = nullptr;

  void
  out (const char *fmt, ...)
  {
    va_list ap;
    va_start (ap, fmt);
    vfprintf (g_out, fmt, ap);
    va_end (ap);
    fflush (g_out);
  }

  uint64_t
  fnv (const char *p, size_t n)
  {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i)
      {
	h ^= (unsigned char) p[i];
	h *= 1099511628211ULL;
      }
    return h;
  }

  std::string
  hex (const char *p, size_t n)
  {
    static const char *digits = "0123456789abcdef";
    std::string s;
    s.reserve (n * 2);
    for (size_t i = 0; i < n; ++i)
      {
	s.push_back (digits[ ((unsigned char) p[i]) >> 4]);
	s.push_back (digits[ ((unsigned char) p[i]) & 15]);
      }
    return s;
  }

  int
  run (const std::string &sql)
  {
    const int rc = exec_sql (sql.c_str ());
    const int err = rc < 0 ? er_errid () : 0;
    std::string shown = sql.size () > 160 ? sql.substr (0, 160) + "..." : sql;
    out ("SQL [%s] => rc=%d err=%d\n", shown.c_str (), rc, err);
    if (rc < 0)
      {
	er_clear ();
      }
    return rc;
  }

  void
  commit ()
  {
    const int rc = db_commit_transaction ();
    out ("COMMIT => %d\n", rc);
  }

  unsigned int
  raw_entry (const char *vot, int index, int width)
  {
    if (width == OR_BYTE_SIZE)
      {
	return (unsigned char) vot[index];
      }
    if (width == OR_SHORT_SIZE)
      {
	return ((unsigned int) OR_GET_SHORT (vot + index * 2)) & 0xFFFF;
      }
    return (unsigned int) OR_GET_INT (vot + index * 4);
  }

  void
  dump_record (THREAD_ENTRY *thread, const OID &class_oid, std::vector<char> &bytes, int slot, int ordinal,
	       bool pending_stubs = false)
  {
    RECDES rec = { (int) bytes.size (), (int) bytes.size (), REC_HOME, bytes.data () };
    std::vector<char> masked = bytes;
    std::string pad;		/* original values of the padding bytes and bits that are masked */
    const int len = rec.length;
    const int hdr = OR_HEADER_SIZE (rec.data);
    const int width = OR_GET_OFFSET_SIZE (rec.data);
    const int repid = OR_GET_MVCC_REPID (rec.data);
    const int flags = OR_GET_RECORD_FLAGS (rec.data);
    const int chn = OR_GET_MVCC_CHN (rec.data);
    const int bound_flag = OR_GET_BOUND_BIT_FLAG (rec.data) ? 1 : 0;

    out ("  row#%d slot=%d len=%d hdr=%d width=%d flags=0x%x repid=%d chn=%d boundflag=%d\n", ordinal, slot, len, hdr,
	 width, flags, repid, chn, bound_flag);

    for (int i = 8; i < hdr && i < len; ++i)
      {
	masked[i] = (char) 0xEE;
      }

    int cache_index = -1;
    OID cls = class_oid;
    OR_CLASSREP *repr = heap_classrepr_get (thread, &cls, nullptr, repid, &cache_index);
    if (repr == nullptr)
      {
	out ("    <no classrepr for repid %d>\n", repid);
	er_clear ();
	return;
      }
    scope_exit free_repr ([&] ()
    {
      heap_classrepr_free (repr, &cache_index);
    });

    const int nvar = repr->n_variable;
    const int nfixed = repr->n_attributes - repr->n_variable;
    const int vot_size = OR_VAR_TABLE_SIZE_INTERNAL (nvar, width);
    const int fixed_start = hdr + vot_size;
    const int bound_start = fixed_start + repr->fixed_length;
    const int bound_size = OR_BOUND_BIT_BYTES (nfixed);
    const int var_start = bound_start + bound_size;

    out ("    nattr=%d nvar=%d nfixed=%d fixed_length=%d vot_size=%d var_start=%d\n", repr->n_attributes, nvar,
	 nfixed, repr->fixed_length, vot_size, var_start);
    if (var_start > len)
      {
	out ("    <record shorter than its layout: var_start=%d len=%d>\n", var_start, len);
	return;
      }

    /* VOT entries (raw, with flags) and padding */
    if (nvar > 0)
      {
	std::string entries;
	for (int i = 0; i <= nvar; ++i)
	  {
	    char buf[32];
	    snprintf (buf, sizeof (buf), "%x ", raw_entry (rec.data + hdr, i, width));
	    entries += buf;
	  }
	out ("    vot: %s\n", entries.c_str ());
	int last = 0;
	if (heap_recdes_get_var_offset_entry (&rec, nvar, &last) == NO_ERROR)
	  {
	    out ("    endcheck=%s (last=%d hdr=%d len=%d)\n", hdr + OR_GET_VAR_OFFSET (last) == len ? "ok" : "BAD",
		 OR_GET_VAR_OFFSET (last), hdr, len);
	  }
	else
	  {
	    out ("    endcheck=<read error %d>\n", er_errid ());
	    er_clear ();
	  }
	for (int i = hdr + (nvar + 1) * width; i < fixed_start; ++i)
	  {
	    pad.push_back (bytes[i]);
	    masked[i] = 0;
	  }
      }

    /* fixed area: keep bytes owned by a fixed attribute, mask gaps */
    if (repr->fixed_length > 0)
      {
	std::vector<bool> owned (repr->fixed_length, false);
	for (int a = 0; a < repr->n_attributes; ++a)
	  {
	    const OR_ATTRIBUTE &att = repr->attributes[a];
	    if (!att.is_fixed)
	      {
		continue;
	      }
	    const int size = tp_domain_disk_size (att.domain);
	    for (int k = att.location; k < att.location + size && k < repr->fixed_length; ++k)
	      {
		owned[k] = true;
	      }
	    const char *p = rec.data + fixed_start + att.location;
	    const bool bound = !bound_flag || OR_GET_BOUND_BIT (rec.data + bound_start, att.position);
	    if (OID_EQ (&class_oid, oid_Serial_class_oid) && TP_DOMAIN_TYPE (att.domain) == DB_TYPE_DATETIME)
	      {
		/* db_serial created_time / updated_time hold the wall clock of CREATE SERIAL */
		for (int k = 0; k < size; ++k)
		  {
		    masked[fixed_start + att.location + k] = (char) 0xEE;
		  }
		out ("    fixed id=%d loc=%d pos=%d size=%d bound=%d bytes=<masked wall-clock datetime>\n", att.id,
		     att.location, att.position, size, bound ? 1 : 0);
		continue;
	      }
	    if (size <= 32)
	      {
		out ("    fixed id=%d loc=%d pos=%d size=%d bound=%d bytes=%s\n", att.id, att.location, att.position,
		     size, bound ? 1 : 0, hex (p, size).c_str ());
	      }
	    else
	      {
		out ("    fixed id=%d loc=%d pos=%d size=%d bound=%d fnv=%016llx\n", att.id, att.location,
		     att.position, size, bound ? 1 : 0, (unsigned long long) fnv (p, size));
	      }
	  }
	for (int k = 0; k < repr->fixed_length; ++k)
	  {
	    if (!owned[k])
	      {
		pad.push_back (bytes[fixed_start + k]);
		masked[fixed_start + k] = 0;
	      }
	  }
      }

    /* bound bits: keep only the bits of real fixed attributes */
    if (bound_size > 0)
      {
	for (int bit = 0; bit < bound_size * 8; ++bit)
	  {
	    if (bit >= nfixed)
	      {
		pad.push_back ((bytes[bound_start + (bit >> 3)] >> (bit & 7)) & 1);
		masked[bound_start + (bit >> 3)] &= (char) ~ (1 << (bit & 7));
	      }
	  }
	out ("    boundbits(masked)=%s\n", hex (masked.data () + bound_start, bound_size).c_str ());
      }

    /* variable fields */
    int oos_entries = 0;
    for (int a = 0; a < repr->n_attributes; ++a)
      {
	const OR_ATTRIBUTE &att = repr->attributes[a];
	if (att.is_fixed)
	  {
	    continue;
	  }
	int start = 0, end = 0;
	if (heap_recdes_get_var_offset_entry (&rec, att.location, &start) != NO_ERROR
	    || heap_recdes_get_var_offset_entry (&rec, att.location + 1, &end) != NO_ERROR)
	  {
	    out ("    var id=%d loc=%d <offset read error %d>\n", att.id, att.location, er_errid ());
	    er_clear ();
	    continue;
	  }
	const int s = hdr + OR_GET_VAR_OFFSET (start);
	const int e = hdr + OR_GET_VAR_OFFSET (end);
	if (OR_IS_OOS (start))
	  {
	    ++oos_entries;
	  }
	if (OR_IS_OOS (start) && pending_stubs)
	  {
	    out ("    var id=%d loc=%d OOS(pending) field=%d stub=%s\n", att.id, att.location, e - s,
		 hex (rec.data + s, e - s >= 0 && e - s <= 64 ? e - s : 0).c_str ());
	  }
	else if (OR_IS_OOS (start))
	  {
	    oos_chain_ref ref;
	    DB_BIGINT oos_len = 0;
	    if (heap_oos_parse_inline_ref (&rec, att.location, &ref, &oos_len) != NO_ERROR)
	      {
		out ("    var id=%d loc=%d OOS <parse error %d>\n", att.id, att.location, er_errid ());
		er_clear ();
		continue;
	      }
	    for (int k = 0; k < 8 && s + k < len; ++k)
	      {
		masked[s + k] = (char) 0xEE;
	      }
	    for (int k = 16; k < 24 && s + k < len; ++k)
	      {
		masked[s + k] = (char) 0xEE;
	      }
	    std::vector<char> payload ((size_t) oos_len);
	    int rc = oos_read (thread, ref, oos_buffer (payload.data (), payload.size ()));
	    out ("    var id=%d loc=%d OOS field=%d len=%lld read_rc=%d payload_fnv=%016llx\n", att.id, att.location,
		 e - s, (long long) oos_len, rc, (unsigned long long) fnv (payload.data (), payload.size ()));
	    if (rc != NO_ERROR)
	      {
		er_clear ();
	      }
	  }
	else
	  {
	    const int n = e - s;
	    if (n >= 0 && n <= 64)
	      {
		out ("    var id=%d loc=%d inline len=%d bytes=%s\n", att.id, att.location, n, hex (rec.data + s,
		     n).c_str ());
	      }
	    else
	      {
		out ("    var id=%d loc=%d inline len=%d fnv=%016llx\n", att.id, att.location, n,
		     (unsigned long long) (n > 0 ? fnv (rec.data + s, n) : 0));
	      }
	  }
      }

    const bool flag_oos = OR_RECORD_HAS_OOS (rec.data);
    out ("    oos_entries=%d has_oos_flag=%d %s\n", oos_entries, flag_oos ? 1 : 0,
	 (oos_entries > 0) == flag_oos ? "oos_consistent" : "OOS_FLAG_MISMATCH");
    EXPECT_EQ (oos_entries > 0, flag_oos);
    out ("    masked_fnv=%016llx\n", (unsigned long long) fnv (masked.data (), masked.size ()));
    out ("    padding_bytes=%zu padding_nonzero=%d padding_fnv=%016llx\n", pad.size (),
	 (int) std::count_if (pad.begin (), pad.end (), [] (char c)
    {
      return c != 0;
    }), (unsigned long long) fnv (pad.data (), pad.size ()));
    if (len <= 512)
      {
	out ("    masked_hex=%s\n", hex (masked.data (), masked.size ()).c_str ());
      }
  }

  void
  dump_class (const char *name)
  {
    auto *thread = thread_get_thread_entry_info ();
    out ("TABLE %s\n", name);
    DB_OBJECT *cls = db_find_class (name);
    if (cls == nullptr)
      {
	out ("  <no class, err=%d>\n", er_errid ());
	er_clear ();
	return;
      }
    OID class_oid = *WS_OID (cls);
    if (strcmp (name, "db_serial") == 0)
      {
	class_oid = *oid_Serial_class_oid;
      }
    HFID hfid;
    HFID_SET_NULL (&hfid);
    const int hfid_rc = heap_get_class_hfid (thread, &class_oid, &hfid, nullptr);
    if (hfid_rc != NO_ERROR || HFID_IS_NULL (&hfid))
      {
	out ("  <no heap rc=%d err=%d class=%d|%d|%d>\n", hfid_rc, er_errid (), class_oid.volid, class_oid.pageid,
	     class_oid.slotid);
	er_clear ();
	return;
      }

    std::vector<std::vector<char>> records;
    std::vector<int> slots;
    HEAP_SCANCACHE scan;
    if (heap_scancache_start (thread, &scan, &hfid, &class_oid, true, logtb_get_mvcc_snapshot (thread)) != NO_ERROR)
      {
	out ("  <scancache start failed %d>\n", er_errid ());
	er_clear ();
	return;
      }
    OID oid;
    OID_SET_NULL (&oid);
    while (true)
      {
	RECDES rec = RECDES_INITIALIZER;
	OID scan_class = class_oid;
	const SCAN_CODE sc = heap_next (thread, &hfid, &scan_class, &oid, &rec, &scan, COPY,
					HEAP_RECDES_DONT_CONSUME_RAW_BYTES);
	if (sc != S_SUCCESS)
	  {
	    if (sc != S_END)
	      {
		out ("  <heap_next => %d err=%d>\n", (int) sc, er_errid ());
		er_clear ();
	      }
	    break;
	  }
	records.emplace_back (rec.data, rec.data + rec.length);
	slots.push_back (oid.slotid);
      }
    heap_scancache_end (thread, &scan);

    out ("  rows=%zu\n", records.size ());
    for (size_t i = 0; i < records.size (); ++i)
      {
	dump_record (thread, class_oid, records[i], slots[i], (int) i);
      }

    const DISK_ISVALID valid = xboot_checkdb_table (thread, CHECKDB_ALL_CHECK_EXCEPT_PREV_LINK, &class_oid, NULL);
    out ("  checkdb=%d\n", (int) valid);
    EXPECT_EQ (valid, DISK_VALID) << name;
    er_clear ();
  }

  DB_OBJECT *
  find_by_id (const char *table, int id)
  {
    DB_OBJECT *cls = db_find_class (table);
    if (cls == nullptr)
      {
	return nullptr;
      }
    DB_OBJLIST *objects = db_fetch_all_objects (cls, DB_FETCH_WRITE);
    DB_OBJECT *found = nullptr;
    for (DB_OBJLIST *item = objects; item != nullptr; item = item->next)
      {
	DB_VALUE v;
	db_make_null (&v);
	if (db_get (item->op, "id", &v) == NO_ERROR && !DB_IS_NULL (&v) && db_get_int (&v) == id)
	  {
	    found = item->op;
	  }
	db_value_clear (&v);
      }
    if (objects != nullptr)
      {
	db_objlist_free (objects);
      }
    return found;
  }

  int
  put_string (DB_OBJECT *obj, const char *attr, const std::string *value)
  {
    DB_VALUE v;
    if (value == nullptr)
      {
	db_make_null (&v);
      }
    else
      {
	db_make_varchar (&v, DB_MAX_VARCHAR_PRECISION, value->c_str (), (int) value->size (), LANG_SYS_CODESET,
			 LANG_SYS_COLLATION);
      }
    const int rc = db_put (obj, attr, &v);
    out ("  db_put %s len=%d => %d\n", attr, value ? (int) value->size () : -1, rc);
    if (rc != NO_ERROR)
      {
	er_clear ();
      }
    return rc;
  }

  int
  put_int (DB_OBJECT *obj, const char *attr, int value)
  {
    DB_VALUE v;
    db_make_int (&v, value);
    const int rc = db_put (obj, attr, &v);
    out ("  db_put %s=%d => %d\n", attr, value, rc);
    if (rc != NO_ERROR)
      {
	er_clear ();
      }
    return rc;
  }

  std::string
  num (int n)
  {
    return std::to_string (n);
  }

  /* deterministic, poorly compressible text */
  std::string
  noise (int n, unsigned seed)
  {
    std::string s;
    s.reserve ((size_t) n);
    unsigned x = seed * 2654435761u + 1;
    for (int i = 0; i < n; ++i)
      {
	x = x * 1103515245u + 12345u;
	s.push_back ((char) ('A' + ((x >> 16) % 26)));
      }
    return s;
  }

  struct stored_row
  {
    OID oid;
    std::vector<char> bytes;
  };

  bool
  collect_rows (const char *name, OID *class_oid_out, std::vector<stored_row> &rows)
  {
    auto *thread = thread_get_thread_entry_info ();
    DB_OBJECT *cls = db_find_class (name);
    if (cls == nullptr)
      {
	er_clear ();
	return false;
      }
    OID class_oid = *WS_OID (cls);
    if (strcmp (name, "db_serial") == 0)
      {
	class_oid = *oid_Serial_class_oid;
      }
    HFID hfid;
    if (heap_get_class_hfid (thread, &class_oid, &hfid, nullptr) != NO_ERROR || HFID_IS_NULL (&hfid))
      {
	er_clear ();
	return false;
      }
    HEAP_SCANCACHE scan;
    if (heap_scancache_start (thread, &scan, &hfid, &class_oid, true, logtb_get_mvcc_snapshot (thread)) != NO_ERROR)
      {
	er_clear ();
	return false;
      }
    OID oid;
    OID_SET_NULL (&oid);
    while (true)
      {
	RECDES rec = RECDES_INITIALIZER;
	OID scan_class = class_oid;
	if (heap_next (thread, &hfid, &scan_class, &oid, &rec, &scan, COPY, HEAP_RECDES_DONT_CONSUME_RAW_BYTES)
	    != S_SUCCESS)
	  {
	    break;
	  }
	rows.push_back ({ oid, std::vector<char> (rec.data, rec.data + rec.length) });
      }
    heap_scancache_end (thread, &scan);
    er_clear ();
    *class_oid_out = class_oid;
    return true;
  }

  /* Build insert and update images of the current attr_info through both builder entries and dump them. */
  void
  build_all (THREAD_ENTRY *thread, const OID &class_oid, HEAP_CACHE_ATTRINFO *ai, RECDES *old, const char *label,
	     bool pending_path)
  {
    for (int upd = 0; upd < 2; ++upd)
      {
	RECDES *old_recdes = upd ? old : nullptr;
	if (pending_path)
	  {
	    heap_pending_record pending;
	    const SCAN_CODE sc = heap_attrinfo_prepare_record (thread, ai, old_recdes, &pending, false);
	    out ("  build %s %s pending => %d err=%d\n", label, upd ? "update" : "insert", (int) sc,
		 sc == S_SUCCESS ? 0 : er_errid ());
	    if (sc == S_SUCCESS)
	      {
		const RECDES &r = pending.get_recdes ();
		std::vector<char> bytes (r.data, r.data + r.length);
		dump_record (thread, class_oid, bytes, -1, 0, true);
	      }
	    er_clear ();
	  }
	record_descriptor built;
	const SCAN_CODE sc = heap_attrinfo_transform_to_disk (thread, ai, old_recdes, &built);
	out ("  build %s %s legacy => %d err=%d\n", label, upd ? "update" : "insert", (int) sc,
	     sc == S_SUCCESS ? 0 : er_errid ());
	if (sc == S_SUCCESS)
	  {
	    const RECDES &r = built.get_recdes ();
	    std::vector<char> bytes (r.data, r.data + r.length);
	    dump_record (thread, class_oid, bytes, -1, 0, false);
	  }
	er_clear ();
      }
  }

  /* For each stored row: read all values, build unchanged images, then rebuild with attribute `vattr`
   * set to each test size (first `vrows` rows only). Optionally mark `incr_attr` for INCR(). */
  void
  direct_class (const char *name, const char *vattr, const std::vector<int> &sizes, int vrows, bool pending_path,
		const char *incr_attr = nullptr)
  {
    auto *thread = thread_get_thread_entry_info ();
    out ("DIRECT %s\n", name);
    OID class_oid;
    std::vector<stored_row> rows;
    if (!collect_rows (name, &class_oid, rows))
      {
	out ("  <no rows>\n");
	return;
      }
    ATTR_ID vid = NULL_ATTRID, iid = NULL_ATTRID;
    DB_OBJECT *cls = db_find_class (name);
    if (vattr != nullptr && cls != nullptr && db_get_attribute (cls, vattr) != nullptr)
      {
	vid = db_attribute_id (db_get_attribute (cls, vattr));
      }
    if (incr_attr != nullptr && cls != nullptr && db_get_attribute (cls, incr_attr) != nullptr)
      {
	iid = db_attribute_id (db_get_attribute (cls, incr_attr));
      }
    er_clear ();
    int ordinal = 0;
    for (stored_row &row : rows)
      {
	RECDES rec = { (int) row.bytes.size (), (int) row.bytes.size (), REC_HOME, row.bytes.data () };
	HEAP_CACHE_ATTRINFO ai;
	if (heap_attrinfo_start (thread, &class_oid, -1, nullptr, &ai) != NO_ERROR)
	  {
	    out ("  <attrinfo start failed>\n");
	    er_clear ();
	    return;
	  }
	scope_exit end_ai ([&] ()
	{
	  heap_attrinfo_end (thread, &ai);
	});
	const int rd = heap_attrinfo_read_dbvalues (thread, &row.oid, &rec, &ai);
	out (" row#%d slot=%d read=%d num_values=%d\n", ordinal, row.oid.slotid, rd, ai.num_values);
	if (rd != NO_ERROR)
	  {
	    er_clear ();
	    ++ordinal;
	    continue;
	  }
	build_all (thread, class_oid, &ai, &rec, "unchanged", pending_path);
	if (iid != NULL_ATTRID)
	  {
	    HEAP_ATTRVALUE *v = heap_attrvalue_locate (iid, &ai);
	    if (v != nullptr)
	      {
		v->do_increment = 1;
		build_all (thread, class_oid, &ai, &rec, "incr", false);
		v->do_increment = -1;
		build_all (thread, class_oid, &ai, &rec, "decr", false);
		v->do_increment = 0;
	      }
	  }
	if (vid != NULL_ATTRID && ordinal < vrows)
	  {
	    for (int size : sizes)
	      {
		DB_VALUE v;
		std::string text;
		if (size < 0)
		  {
		    db_make_null (&v);
		  }
		else
		  {
		    text = noise (size, (unsigned) (size + ordinal));
		    db_make_varchar (&v, DB_MAX_VARCHAR_PRECISION, text.c_str (), (int) text.size (), LANG_SYS_CODESET,
				     LANG_SYS_COLLATION);
		  }
		const int src = heap_attrinfo_set (&row.oid, vid, &v, &ai);
		const std::string label = std::string (vattr) + "=" + num (size);
		out ("  set %s => %d\n", label.c_str (), src);
		if (src != NO_ERROR)
		  {
		    er_clear ();
		    continue;
		  }
		build_all (thread, class_oid, &ai, &rec, label.c_str (), pending_path);
	      }
	  }
	++ordinal;
      }
  }
}

class WriteDiff : public ::testing::Test
{
};

// 1-byte <-> 2-byte variable offsets, INSERT and UPDATE that grow or shrink across the width.
TEST_F (WriteDiff, OffsetWidth12)
{
  out ("=== OffsetWidth12\n");
  run ("CREATE TABLE wd_w12 (id INT, a VARCHAR, b VARCHAR, c INT, INDEX i_w12 (id, a))");
  for (int n = 70; n <= 115; ++n)
    {
      run ("INSERT INTO wd_w12 VALUES (" + num (n) + ", REPEAT('a', " + num (n) + "), 'b', "
	   + (n % 4 == 0 ? std::string ("NULL") : num (n)) + ")");
    }
  commit ();
  run ("UPDATE wd_w12 SET a = REPEAT('u', 185 - id) WHERE id % 3 = 0");
  run ("UPDATE wd_w12 SET a = REPEAT('s', id - 15), b = NULL WHERE id % 3 = 1");
  run ("UPDATE wd_w12 SET c = 7 WHERE id % 3 = 2");
  commit ();
  run ("UPDATE wd_w12 SET a = REPEAT('v', id + 1) WHERE id % 5 = 0");
  commit ();
  dump_class ("wd_w12");
}

// 2-byte <-> 4-byte variable offsets with a large fixed BIT column; variable values stay <= 24 bytes
// so they are never OOS-eligible.
TEST_F (WriteDiff, OffsetWidth24)
{
  out ("=== OffsetWidth24\n");
  run ("CREATE TABLE wd_w24 (id INT, f BIT(261760), a VARCHAR, b VARCHAR)");
  for (int n = 0; n <= 22; ++n)
    {
      const std::string f = n % 2 == 0 ? "NULL" : "CAST(REPEAT(X'AB', 32720) AS BIT(261760))";
      const std::string a = n % 7 == 3 ? "NULL" : "REPEAT('x', " + num (n) + ")";
      const std::string b = n % 5 == 0 ? "NULL" : "REPEAT('y', " + num (n % 20) + ")";
      run ("INSERT INTO wd_w24 VALUES (" + num (n) + ", " + f + ", " + a + ", " + b + ")");
    }
  commit ();
  run ("UPDATE wd_w24 SET a = REPEAT('z', 22 - id) WHERE id % 2 = 0");
  run ("UPDATE wd_w24 SET b = NULL, a = NULL WHERE id % 3 = 0");
  run ("UPDATE wd_w24 SET b = REPEAT('w', 20) WHERE id % 3 = 1");
  commit ();
  dump_class ("wd_w24");
}

// FORCE_OUTLINE threshold, NULL forced values, forced demotion changing the offset width.
TEST_F (WriteDiff, ForceOutline)
{
  out ("=== ForceOutline\n");
  run ("CREATE TABLE wd_force (id INT, a VARCHAR STORAGE FORCE_OUTLINE, b BIT VARYING STORAGE FORCE_OUTLINE, "
       "c VARCHAR, INDEX i_force (id, c))");
  const int bsizes[] = { -1, 0, 20, 21, 24, 25, 200, 70000 };
  const int csizes[] = { 0, 90, 100, 200 };
  int id = 0;
  for (int n = 15; n <= 34; ++n)
    {
      const int bs = bsizes[n % 8];
      const int cs = csizes[n % 4];
      const std::string b = bs < 0 ? "NULL" : "CAST(REPEAT(X'BB', " + num (bs) + ") AS BIT VARYING)";
      run ("INSERT INTO wd_force VALUES (" + num (id++) + ", REPEAT('a', " + num (n) + "), " + b + ", REPEAT('c', "
	   + num (cs) + "))");
    }
  run ("INSERT INTO wd_force VALUES (" + num (id++) + ", NULL, NULL, NULL)");
  run ("INSERT INTO wd_force VALUES (" + num (id++) + ", '', CAST(X'' AS BIT VARYING), '')");
  run ("INSERT INTO wd_force VALUES (" + num (id++) + ", REPEAT('a', 65536), NULL, REPEAT('c', 120))");
  commit ();
  run ("UPDATE wd_force SET a = NULL WHERE id % 4 = 0");
  run ("UPDATE wd_force SET a = 'tiny' WHERE id % 4 = 1");
  run ("UPDATE wd_force SET b = CAST(REPEAT(X'CD', 70000) AS BIT VARYING) WHERE id % 4 = 2");
  run ("UPDATE wd_force SET c = REPEAT('d', 300) WHERE id % 4 = 3");
  commit ();
  run ("UPDATE wd_force SET a = REPEAT('e', 40) WHERE id % 4 = 0");
  run ("UPDATE wd_force SET b = CAST(X'01' AS BIT VARYING) WHERE id % 4 = 2");
  run ("UPDATE wd_force SET c = 'x' WHERE id % 4 = 3");
  commit ();
  dump_class ("wd_force");
}

// PREFER_INLINE demotion order, demotion that changes the offset width, and back to inline.
TEST_F (WriteDiff, PreferInlineDemotion)
{
  out ("=== PreferInlineDemotion\n");
  run ("CREATE TABLE wd_pref (id INT, p VARCHAR STORAGE PREFER_INLINE, d1 VARCHAR, d2 BIT VARYING, s VARCHAR)");
  const int psz[] = { 10, 3000, 5000, 1000, 2200, 4100 };
  const int d1sz[] = { 10, 3000, 2000, 4500, 2200, 4100 };
  const int d2sz[] = { 10, 1500, 2000, 3000, 2200, 4100 };
  for (int i = 0; i < 6; ++i)
    {
      for (int sv = 0; sv < 2; ++sv)
	{
	  const int rid = i * 2 + sv;
	  run ("INSERT INTO wd_pref VALUES (" + num (rid) + ", REPEAT('p', " + num (psz[i]) + "), REPEAT('d', "
	       + num (d1sz[i]) + "), CAST(REPEAT(X'D2', " + num (d2sz[i]) + ") AS BIT VARYING), "
	       + (sv == 0 ? std::string ("'s'") : std::string ("REPEAT('s', 150)")) + ")");
	}
    }
  commit ();
  run ("UPDATE wd_pref SET d1 = 'small' WHERE id % 3 = 0");
  run ("UPDATE wd_pref SET p = REPEAT('q', 4000) WHERE id % 3 = 1");
  run ("UPDATE wd_pref SET s = REPEAT('t', 20) WHERE id % 3 = 2");
  commit ();
  run ("UPDATE wd_pref SET p = 'p', d1 = 'd', d2 = CAST(X'0F' AS BIT VARYING) WHERE id % 4 = 0");
  commit ();
  dump_class ("wd_pref");
}

// Exactly 7, 8, 9 attributes and many attributes (stack array of 8 sizes vs heap vector).
TEST_F (WriteDiff, AttributeCounts)
{
  out ("=== AttributeCounts\n");
  run ("CREATE TABLE wd_a7 (id INT, f1 BIGINT, f2 DOUBLE, a VARCHAR, b VARCHAR STORAGE FORCE_OUTLINE, "
       "c BIT VARYING, d VARCHAR STORAGE PREFER_INLINE)");
  run ("CREATE TABLE wd_a8 (id INT, f1 BIGINT, f2 DOUBLE, a VARCHAR, b VARCHAR STORAGE FORCE_OUTLINE, "
       "c BIT VARYING, d VARCHAR STORAGE PREFER_INLINE, e SMALLINT)");
  run ("CREATE TABLE wd_a9 (id INT, f1 BIGINT, f2 DOUBLE, a VARCHAR, b VARCHAR STORAGE FORCE_OUTLINE, "
       "c BIT VARYING, d VARCHAR STORAGE PREFER_INLINE, e SMALLINT, g VARCHAR, INDEX i_a9 (id, a, d))");
  const char *tables[] = { "wd_a7", "wd_a8", "wd_a9" };
  for (int t = 0; t < 3; ++t)
    {
      for (int r = 0; r < 8; ++r)
	{
	  std::string v = num (r) + ", " + (r % 3 == 0 ? "NULL" : num (r * 1000)) + ", "
			  + (r % 4 == 1 ? "NULL" : num (r) + ".5") + ", REPEAT('a', " + num (r * 37) + "), "
			  + (r % 2 == 0 ? "NULL" : "REPEAT('b', " + num (r * 11) + ")") + ", CAST(REPEAT(X'CC', "
			  + num (r * 900) + ") AS BIT VARYING), REPEAT('d', " + num (r * 700) + ")";
	  if (t >= 1)
	    {
	      v += ", " + (r % 2 == 1 ? std::string ("NULL") : num (r));
	    }
	  if (t >= 2)
	    {
	      v += ", " + (r % 3 == 2 ? std::string ("NULL") : "REPEAT('g', " + num (r * 300) + ")");
	    }
	  run (std::string ("INSERT INTO ") + tables[t] + " VALUES (" + v + ")");
	}
    }
  commit ();
  for (int t = 0; t < 3; ++t)
    {
      const std::string tb = tables[t];
      run ("UPDATE " + tb + " SET a = REPEAT('A', 6000) WHERE id % 3 = 0");
      run ("UPDATE " + tb + " SET c = NULL, d = 'x' WHERE id % 3 = 1");
      run ("UPDATE " + tb + " SET f1 = NULL, f2 = 1.25, b = REPEAT('B', 30) WHERE id % 3 = 2");
    }
  commit ();

  // many attributes: 40 fixed and 40 variable
  std::string cols = "id INT";
  for (int i = 0; i < 40; ++i)
    {
      cols += ", f" + num (i) + (i % 3 == 0 ? " BIGINT" : i % 3 == 1 ? " INT" : " SMALLINT");
    }
  for (int i = 0; i < 40; ++i)
    {
      cols += ", v" + num (i) + (i % 5 == 0 ? " VARCHAR STORAGE FORCE_OUTLINE" : i % 5 == 1 ?
				 " VARCHAR STORAGE PREFER_INLINE" : " VARCHAR");
    }
  run ("CREATE TABLE wd_many (" + cols + ")");
  for (int r = 0; r < 6; ++r)
    {
      std::string v = num (r);
      for (int i = 0; i < 40; ++i)
	{
	  v += (i + r) % 4 == 0 ? ", NULL" : ", " + num (i * 10 + r);
	}
      for (int i = 0; i < 40; ++i)
	{
	  v += (i + r) % 6 == 0 ? ", NULL" : ", REPEAT('v', " + num ((i * 13 + r * 29) % 400) + ")";
	}
      run ("INSERT INTO wd_many VALUES (" + v + ")");
    }
  commit ();
  run ("UPDATE wd_many SET v3 = REPEAT('w', 5000), f7 = NULL WHERE id % 2 = 0");
  run ("UPDATE wd_many SET v0 = 'tiny', f8 = 99 WHERE id % 2 = 1");
  commit ();
  for (int t = 0; t < 3; ++t)
    {
      dump_class (tables[t]);
    }
  dump_class ("wd_many");
}

// NULL fixed columns, every fixed width, bound bits across a 32-bit word, fixed next to variable.
TEST_F (WriteDiff, FixedAndBoundBits)
{
  out ("=== FixedAndBoundBits\n");
  run ("CREATE TABLE wd_fixed (id INT, s SMALLINT, v1 VARCHAR, i INT, bi BIGINT, fl FLOAT, d DOUBLE, "
       "dt DATE, tm TIME, ts TIMESTAMP, dtt DATETIME, n NUMERIC(10,2), m MONETARY, bt BIT(17), "
       "v2 VARCHAR STORAGE FORCE_OUTLINE, n38 NUMERIC(38,5))");
  const char *rows[] =
  {
    "1, 2, 'x', 3, 4, 5.5, 6.25, DATE'2020-01-02', TIME'03:04:05', TIMESTAMP'2020-01-02 03:04:05', "
    "DATETIME'2020-01-02 03:04:05.678', 12.34, 5.67, B'10101010101010101', REPEAT('v', 100), 123456789.12345",
    "2, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL",
    "3, NULL, 'y', 3, NULL, 5.5, NULL, DATE'2020-01-02', NULL, TIMESTAMP'2020-01-02 03:04:05', NULL, 12.34, NULL, "
    "B'1', REPEAT('w', 5000), NULL",
    "4, 7, REPEAT('z', 300), NULL, 8, NULL, 9.0, NULL, TIME'00:00:00', NULL, DATETIME'1999-12-31 23:59:59.999', "
    "NULL, 1.0, NULL, NULL, -1.5",
  };
  for (const char *r : rows)
    {
      run (std::string ("INSERT INTO wd_fixed VALUES (") + r + ")");
    }
  commit ();
  run ("UPDATE wd_fixed SET s = NULL, bi = 77, v1 = REPEAT('q', 200) WHERE id = 1");
  run ("UPDATE wd_fixed SET s = 1, n = 1.5, bt = B'11' WHERE id = 2");
  run ("UPDATE wd_fixed SET v2 = NULL, ts = NULL WHERE id = 3");
  run ("UPDATE wd_fixed SET n38 = NULL, v2 = REPEAT('k', 70000) WHERE id = 4");
  commit ();

  std::string cols = "id INT";
  for (int i = 0; i < 33; ++i)
    {
      cols += ", c" + num (i) + " INT";
    }
  cols += ", v VARCHAR";
  run ("CREATE TABLE wd_bb33 (" + cols + ")");
  for (int r = 0; r < 5; ++r)
    {
      std::string v = num (r);
      for (int i = 0; i < 33; ++i)
	{
	  v += (i * 7 + r) % 3 == 0 ? ", NULL" : ", " + num (i + r);
	}
      v += r % 2 == 0 ? ", NULL" : ", REPEAT('v', " + num (r * 1500) + ")";
      run ("INSERT INTO wd_bb33 VALUES (" + v + ")");
    }
  commit ();
  run ("UPDATE wd_bb33 SET c32 = NULL, c0 = 5 WHERE id % 2 = 0");
  run ("UPDATE wd_bb33 SET c31 = 1, c32 = 2, v = REPEAT('V', 4800) WHERE id % 2 = 1");
  commit ();
  dump_class ("wd_fixed");
  dump_class ("wd_bb33");
}

// Click counters (do_increment) through the fixed-column pointer, with VOT and OOS present.
TEST_F (WriteDiff, ClickCounters)
{
  out ("=== ClickCounters\n");
  run ("CREATE TABLE wd_incr (id INT PRIMARY KEY, cnt INT, scnt SMALLINT, bcnt BIGINT, a VARCHAR, "
       "big VARCHAR STORAGE FORCE_OUTLINE, mid VARCHAR, g1 INT, g2 VARCHAR)");
  run ("INSERT INTO wd_incr VALUES (1, 10, 5, 100, 'a', REPEAT('B', 9000), REPEAT('m', 200), 1, 'g')");
  run ("INSERT INTO wd_incr VALUES (2, 20, 6, NULL, 'b', NULL, REPEAT('m', 10), NULL, NULL)");
  run ("INSERT INTO wd_incr VALUES (3, NULL, NULL, 300, REPEAT('a', 6000), REPEAT('B', 30), 'm', 3, "
       "REPEAT('g', 5000))");
  run ("INSERT INTO wd_incr VALUES (4, 0, 0, 0, NULL, NULL, NULL, NULL, NULL)");
  commit ();
  for (int k = 0; k < 3; ++k)
    {
      run ("SELECT INCR(cnt) FROM wd_incr WHERE id = 1");
      run ("SELECT DECR(scnt) FROM wd_incr WHERE id = 2");
      run ("SELECT INCR(bcnt) FROM wd_incr WHERE id = 3");
      run ("SELECT INCR(cnt) FROM wd_incr WHERE id = 3");
      run ("SELECT a, INCR(g1) FROM wd_incr WHERE id = 1");
      run ("SELECT DECR(cnt) FROM wd_incr WHERE id = 4");
    }
  commit ();
  dump_class ("wd_incr");
}

// Non-MVCC class: db_serial records rewritten by NEXT_VALUE and AUTO_INCREMENT.
TEST_F (WriteDiff, SerialNonMvcc)
{
  out ("=== SerialNonMvcc\n");
  run ("CREATE SERIAL wd_s1 START WITH 1 INCREMENT BY 1 NOCACHE");
  run ("CREATE SERIAL wd_s2 START WITH 100 INCREMENT BY -3 MINVALUE -1000 NOCACHE");
  run ("CREATE SERIAL wd_s3 START WITH 1 INCREMENT BY 1000000000 MAXVALUE 99999999999999999999 NOCACHE");
  run ("CREATE TABLE wd_ai (id NUMERIC(10) AUTO_INCREMENT, a VARCHAR)");
  run ("CREATE TABLE wd_ai2 (id BIGINT AUTO_INCREMENT, a VARCHAR, big VARCHAR STORAGE FORCE_OUTLINE)");
  commit ();
  for (int k = 0; k < 3; ++k)
    {
      run ("SELECT wd_s1.NEXT_VALUE FROM db_root");
      run ("SELECT wd_s2.NEXT_VALUE FROM db_root");
      run ("SELECT wd_s3.NEXT_VALUE FROM db_root");
      run ("INSERT INTO wd_ai (a) VALUES ('r" + num (k) + "')");
      run ("INSERT INTO wd_ai2 (a, big) VALUES ('r" + num (k) + "', REPEAT('z', 100))");
    }
  commit ();
  dump_class ("db_serial");
  dump_class ("wd_ai");
  dump_class ("wd_ai2");
}

// UPDATE of rows stored in an older representation (ADD COLUMN after rows exist).
TEST_F (WriteDiff, OldRepresentationUpdate)
{
  out ("=== OldRepresentationUpdate\n");
  run ("CREATE TABLE wd_old (id INT, a VARCHAR, big BIT VARYING, f INT)");
  run ("INSERT INTO wd_old VALUES (1, 'a', CAST(REPEAT(X'AA', 6000) AS BIT VARYING), 1)");
  run ("INSERT INTO wd_old VALUES (2, REPEAT('b', 150), NULL, NULL)");
  run ("INSERT INTO wd_old VALUES (3, 'c', CAST(REPEAT(X'CC', 100) AS BIT VARYING), 3)");
  run ("INSERT INTO wd_old VALUES (4, 'd', CAST(REPEAT(X'DD', 6000) AS BIT VARYING), 4)");
  commit ();
  run ("ALTER TABLE wd_old ADD COLUMN added INT DEFAULT 41");
  run ("ALTER TABLE wd_old ADD COLUMN addv VARCHAR DEFAULT 'dflt'");
  commit ();
  run ("UPDATE wd_old SET a = 'x' WHERE id = 1");
  run ("UPDATE wd_old SET big = CAST(REPEAT(X'BB', 7000) AS BIT VARYING) WHERE id = 2");
  run ("UPDATE wd_old SET f = NULL WHERE id = 3");
  commit ();
  dump_class ("wd_old");
}

// Server-side UPDATE: OOS -> inline -> OOS of the same row, and non-OOS column changes beside OOS.
TEST_F (WriteDiff, ServerUpdateOosRoundTrip)
{
  out ("=== ServerUpdateOosRoundTrip\n");
  run ("CREATE TABLE wd_rt (id INT UNIQUE, a VARCHAR, big BIT VARYING, big2 VARCHAR, INDEX i_rt (id, a))");
  run ("INSERT INTO wd_rt VALUES (1, 'a', CAST(REPEAT(X'11', 6000) AS BIT VARYING), REPEAT('2', 5000))");
  run ("INSERT INTO wd_rt VALUES (2, 'b', CAST(X'22' AS BIT VARYING), 'x')");
  commit ();
  run ("UPDATE wd_rt SET big = CAST(X'33' AS BIT VARYING), big2 = 'y' WHERE id = 1");
  run ("UPDATE wd_rt SET big = CAST(REPEAT(X'44', 6000) AS BIT VARYING) WHERE id = 2");
  commit ();
  run ("UPDATE wd_rt SET big = CAST(REPEAT(X'55', 6000) AS BIT VARYING) WHERE id = 1");
  run ("UPDATE wd_rt SET a = 'only fixed-ish change' WHERE id = 2");
  commit ();
  run ("UPDATE wd_rt SET big = CAST(X'66' AS BIT VARYING) WHERE id = 2");
  run ("UPDATE wd_rt SET a = REPEAT('a', 200) WHERE id = 1");
  commit ();
  dump_class ("wd_rt");
}

// Client workspace UPDATE and INSERT (db_put then commit): OOS <-> inline, width changes.
TEST_F (WriteDiff, WorkspaceUpdate)
{
  out ("=== WorkspaceUpdate\n");
  run ("CREATE TABLE wd_ws (id INT UNIQUE, a VARCHAR, big VARCHAR, f VARCHAR STORAGE FORCE_OUTLINE, n INT)");
  run ("INSERT INTO wd_ws VALUES (1, 'a', REPEAT('B', 6000), REPEAT('F', 100), 1)");
  run ("INSERT INTO wd_ws VALUES (2, 'b', 'small', NULL, NULL)");
  run ("INSERT INTO wd_ws VALUES (3, REPEAT('c', 120), NULL, 'tiny', 3)");
  commit ();

  const std::string big_b = noise (7000, 1), small = "s", wide = noise (200, 2), forced = noise (40, 3);
  DB_OBJECT *o1 = find_by_id ("wd_ws", 1);
  DB_OBJECT *o2 = find_by_id ("wd_ws", 2);
  DB_OBJECT *o3 = find_by_id ("wd_ws", 3);
  out ("  found %d %d %d\n", o1 != nullptr, o2 != nullptr, o3 != nullptr);
  ASSERT_NE (o1, nullptr);
  ASSERT_NE (o2, nullptr);
  ASSERT_NE (o3, nullptr);
  put_string (o1, "big", &small);	/* OOS -> inline */
  put_string (o2, "big", &big_b);	/* inline -> OOS */
  put_string (o3, "a", &wide);
  put_string (o3, "f", &forced);	/* forced selection */
  put_int (o3, "n", 33);
  commit ();

  o1 = find_by_id ("wd_ws", 1);
  o2 = find_by_id ("wd_ws", 2);
  ASSERT_NE (o1, nullptr);
  ASSERT_NE (o2, nullptr);
  put_string (o1, "big", &big_b);	/* inline -> OOS again */
  put_string (o1, "f", nullptr);
  put_string (o2, "big", nullptr);	/* OOS -> NULL */
  commit ();

  DB_OBJECT *cls = db_find_class ("wd_ws");
  ASSERT_NE (cls, nullptr);
  for (int k = 0; k < 2; ++k)
    {
      DB_OBJECT *obj = db_create (cls);
      ASSERT_NE (obj, nullptr);
      put_int (obj, "id", 10 + k);
      put_string (obj, "a", k == 0 ? &small : &wide);
      put_string (obj, "big", k == 0 ? &big_b : nullptr);
      put_string (obj, "f", &forced);
    }
  commit ();
  dump_class ("wd_ws");
}

// Client-side multi-row UPDATE caused by a trigger (copy-area flush, locator_force_for_multi_update).
TEST_F (WriteDiff, TriggerUpdate)
{
  out ("=== TriggerUpdate\n");
  run ("CREATE TABLE wd_trg (id INT, a VARCHAR, big BIT VARYING, f VARCHAR STORAGE FORCE_OUTLINE)");
  run ("CREATE TABLE wd_trg_log (n INT)");
  run ("CREATE TRIGGER wd_trg_u AFTER UPDATE ON wd_trg EXECUTE INSERT INTO wd_trg_log VALUES (1)");
  run ("INSERT INTO wd_trg VALUES (1, 'a', CAST(REPEAT(X'11', 6000) AS BIT VARYING), REPEAT('f', 50))");
  run ("INSERT INTO wd_trg VALUES (2, REPEAT('b', 100), CAST(X'22' AS BIT VARYING), NULL)");
  run ("INSERT INTO wd_trg VALUES (3, NULL, NULL, 'f')");
  commit ();
  run ("UPDATE wd_trg SET big = CAST(X'33' AS BIT VARYING) WHERE id = 1");
  run ("UPDATE wd_trg SET big = CAST(REPEAT(X'44', 6000) AS BIT VARYING), a = REPEAT('b', 10) WHERE id = 2");
  run ("UPDATE wd_trg SET f = REPEAT('g', 30), a = REPEAT('c', 130) WHERE id = 3");
  commit ();
  run ("UPDATE wd_trg SET big = CAST(REPEAT(X'55', 6000) AS BIT VARYING) WHERE id = 1");
  run ("UPDATE wd_trg SET big = NULL WHERE id = 2");
  run ("UPDATE wd_trg SET a = a || 'x'");
  commit ();
  dump_class ("wd_trg");
  dump_class ("wd_trg_log");
}

// UPDATE that moves rows between partitions: server-side, through a trigger, and through the workspace.
TEST_F (WriteDiff, PartitionMove)
{
  out ("=== PartitionMove\n");
  const char *tables[] = { "wd_part", "wd_partt" };
  for (const char *t : tables)
    {
      const std::string tb = t;
      run ("CREATE TABLE " + tb + " (id INT, a VARCHAR, big BIT VARYING, f VARCHAR STORAGE FORCE_OUTLINE) "
	   "PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN (100), PARTITION p1 VALUES LESS THAN MAXVALUE)");
      run ("INSERT INTO " + tb + " VALUES (1, 'a', CAST(REPEAT(X'11', 6000) AS BIT VARYING), REPEAT('f', 40))");
      run ("INSERT INTO " + tb + " VALUES (2, 'b', CAST(X'22' AS BIT VARYING), NULL)");
      run ("INSERT INTO " + tb + " VALUES (3, REPEAT('c', 150), CAST(REPEAT(X'33', 6000) AS BIT VARYING), 'x')");
      run ("INSERT INTO " + tb + " VALUES (4, 'd', NULL, NULL)");
      run ("INSERT INTO " + tb + " VALUES (5, 'e', CAST(REPEAT(X'55', 6000) AS BIT VARYING), NULL)");
    }
  run ("CREATE TABLE wd_partt_log (n INT)");
  run ("CREATE TRIGGER wd_partt_u AFTER UPDATE ON wd_partt EXECUTE INSERT INTO wd_partt_log VALUES (1)");
  commit ();
  for (const char *t : tables)
    {
      const std::string tb = t;
      run ("UPDATE " + tb + " SET id = id + 100 WHERE id = 1");	/* move, OOS unchanged */
      run ("UPDATE " + tb + " SET id = id + 100, big = CAST(REPEAT(X'AA', 6000) AS BIT VARYING) WHERE id = 2");
      run ("UPDATE " + tb + " SET id = id + 100, big = CAST(X'BB' AS BIT VARYING) WHERE id = 3");	/* OOS -> inline */
      run ("UPDATE " + tb + " SET big = CAST(X'CC' AS BIT VARYING) WHERE id = 5");	/* stays */
    }
  commit ();
  for (const char *t : tables)
    {
      const std::string tb = t;
      run ("UPDATE " + tb + " SET id = id - 100, f = REPEAT('g', 50) WHERE id = 101");	/* move back */
    }
  commit ();

  /* workspace update of the partitioning key */
  const std::string big = noise (6000, 4);
  DB_OBJECT *o4 = find_by_id ("wd_part__p__p0", 4);
  out ("  found p0 id=4: %d\n", o4 != nullptr);
  if (o4 != nullptr)
    {
      put_int (o4, "id", 204);
      put_string (o4, "a", &big);
      const int rc = db_commit_transaction ();
      out ("COMMIT(ws move) => %d err=%d\n", rc, rc != NO_ERROR ? er_errid () : 0);
      if (rc != NO_ERROR)
	{
	  er_clear ();
	  db_abort_transaction ();
	}
    }
  const char *parts[] = { "wd_part__p__p0", "wd_part__p__p1", "wd_partt__p__p0", "wd_partt__p__p1" };
  for (const char *p : parts)
    {
      dump_class (p);
    }
  dump_class ("wd_partt_log");
}

// Client-side multi-row UPDATE on classes with a unique index and a trigger: the flush goes through
// locator_flush_for_multi_update / locator_force_for_multi_update, including a partition move.
TEST_F (WriteDiff, TriggerUniqueUpdate)
{
  out ("=== TriggerUniqueUpdate\n");
  run ("CREATE TABLE wd_trgu (id INT UNIQUE, a VARCHAR, big BIT VARYING, f VARCHAR STORAGE FORCE_OUTLINE)");
  run ("CREATE TABLE wd_trgu_log (n INT)");
  run ("CREATE TRIGGER wd_trgu_u AFTER UPDATE ON wd_trgu EXECUTE INSERT INTO wd_trgu_log VALUES (1)");
  run ("CREATE TABLE wd_trgup (id INT PRIMARY KEY, a VARCHAR, big BIT VARYING, f VARCHAR STORAGE FORCE_OUTLINE) "
       "PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN (100), PARTITION p1 VALUES LESS THAN MAXVALUE)");
  run ("CREATE TRIGGER wd_trgup_u AFTER UPDATE ON wd_trgup EXECUTE INSERT INTO wd_trgu_log VALUES (2)");
  const char *tables[] = { "wd_trgu", "wd_trgup" };
  for (const char *t : tables)
    {
      const std::string tb = t;
      run ("INSERT INTO " + tb + " VALUES (1, 'a', CAST(REPEAT(X'11', 6000) AS BIT VARYING), REPEAT('f', 50))");
      run ("INSERT INTO " + tb + " VALUES (2, REPEAT('b', 100), CAST(X'22' AS BIT VARYING), NULL)");
      run ("INSERT INTO " + tb + " VALUES (3, NULL, NULL, 'f')");
      run ("INSERT INTO " + tb + " VALUES (4, 'd', CAST(REPEAT(X'44', 6000) AS BIT VARYING), 'g')");
    }
  commit ();
  for (const char *t : tables)
    {
      const std::string tb = t;
      run ("UPDATE " + tb + " SET big = CASE WHEN id % 2 = 0 THEN CAST(REPEAT(X'77', 6000) AS BIT VARYING) "
	   "ELSE CAST(X'78' AS BIT VARYING) END");
      run ("UPDATE " + tb + " SET f = REPEAT('h', 60), a = REPEAT('a', 130) WHERE id <= 2");
      run ("UPDATE " + tb + " SET big = NULL, a = 'z' WHERE id > 2");
    }
  commit ();
  run ("UPDATE wd_trgup SET id = id + 100, big = CAST(REPEAT(X'99', 6000) AS BIT VARYING) WHERE id <= 2");
  run ("UPDATE wd_trgup SET id = id + 200 WHERE id = 4");
  commit ();
  dump_class ("wd_trgu");
  dump_class ("wd_trgup__p__p0");
  dump_class ("wd_trgup__p__p1");
  dump_class ("wd_trgu_log");
}

// Classes with only variable or only fixed attributes, an attribute-less class, and assorted
// variable types (collections, JSON, ENUM, CHAR, NUMERIC) inline, forced and demoted.
TEST_F (WriteDiff, ShapesAndTypes)
{
  out ("=== ShapesAndTypes\n");
  run ("CREATE TABLE wd_varonly (a VARCHAR, b BIT VARYING, c VARCHAR STORAGE FORCE_OUTLINE)");
  run ("INSERT INTO wd_varonly VALUES ('x', CAST(X'01' AS BIT VARYING), NULL)");
  run ("INSERT INTO wd_varonly VALUES (REPEAT('a', 120), CAST(REPEAT(X'02', 5000) AS BIT VARYING), REPEAT('c', 30))");
  run ("INSERT INTO wd_varonly VALUES (NULL, NULL, NULL)");
  commit ();
  run ("UPDATE wd_varonly SET b = CAST(X'03' AS BIT VARYING) WHERE a = 'x'");
  run ("UPDATE wd_varonly SET a = 'y', b = CAST(REPEAT(X'04', 6000) AS BIT VARYING) WHERE a IS NULL");
  commit ();
  run ("CREATE TABLE wd_fixonly (i INT, s SMALLINT, b BIGINT, d DOUBLE, bt BIT(40))");
  run ("INSERT INTO wd_fixonly VALUES (1, 2, 3, 4.5, B'1010101010101010101010101010101010101010')");
  run ("INSERT INTO wd_fixonly VALUES (NULL, NULL, NULL, NULL, NULL)");
  commit ();
  run ("UPDATE wd_fixonly SET i = 9, bt = NULL WHERE s = 2");
  run ("UPDATE wd_fixonly SET d = 1.0 WHERE i IS NULL");
  commit ();
  run ("SELECT INCR(i) FROM wd_fixonly WHERE s = 2");
  commit ();
  run ("CREATE TABLE wd_noattr");
  run ("INSERT INTO wd_noattr DEFAULT VALUES");
  commit ();
  run ("CREATE TABLE wd_types (id INT, st SET OF INT STORAGE FORCE_OUTLINE, ms MULTISET OF VARCHAR, "
       "sq SEQUENCE OF INT, js JSON, en ENUM('red', 'green', 'blue'), ch CHAR(40), nch VARCHAR(300), "
       "num NUMERIC(30,4), bv BIT VARYING STORAGE PREFER_INLINE)");
  run ("INSERT INTO wd_types VALUES (1, {1,2,3,4,5,6,7,8,9,10}, {'a','b','b'}, {3,2,1}, '{\"k\": [1,2,3]}', 'green', "
       "'char', REPEAT('n', 300), 12345.6789, CAST(REPEAT(X'AB', 100) AS BIT VARYING))");
  run ("INSERT INTO wd_types VALUES (2, {1}, NULL, {}, NULL, NULL, NULL, '', NULL, CAST(REPEAT(X'CD', 5000) AS BIT VARYING))");
  run ("INSERT INTO wd_types VALUES (3, NULL, {'x'}, {1,2}, '[1,2,3]', 'blue', REPEAT('c', 40), REPEAT('m', 10), -1.5, "
       "NULL)");
  commit ();
  run ("UPDATE wd_types SET js = '{\"big\": \"" + std::string (5000, 'j') + "\"}' WHERE id = 1");
  run ("UPDATE wd_types SET st = {7,8,9,10,11,12,13,14,15,16,17,18}, en = 'red' WHERE id = 2");
  run ("UPDATE wd_types SET bv = CAST(REPEAT(X'EF', 4500) AS BIT VARYING), ch = NULL WHERE id = 3");
  commit ();
  dump_class ("wd_varonly");
  dump_class ("wd_fixonly");
  dump_class ("wd_noattr");
  dump_class ("wd_types");
}

// Direct builder images: MVCC insert header (16), MVCC update header (24), non-MVCC (8); pending and
// legacy OOS paths; attribute values that cross every offset width, the FORCE_OUTLINE threshold and
// the DB_PAGESIZE/4 demotion gate; INCR/DECR through do_increment. Run after the SQL scenarios.
TEST_F (WriteDiff, ZDirectBuilder)
{
  out ("=== ZDirectBuilder\n");
  const std::vector<int> sizes = { -1, 0, 1, 20, 21, 22, 23, 24, 25, 60, 90, 94, 95, 96, 97, 98, 100, 110, 127,
				   128, 3000, 4000, 4100, 8000, 32700, 32760, 32800, 70000
				 };
  const std::vector<int> small_sizes = { -1, 0, 18, 20, 21, 22, 23, 24, 30 };
  direct_class ("wd_w12", "a", sizes, 2, true);
  direct_class ("wd_w24", "a", small_sizes, 3, true);
  direct_class ("wd_force", "a", sizes, 2, true);
  direct_class ("wd_force", "c", sizes, 1, true);
  direct_class ("wd_pref", "p", sizes, 2, true);
  direct_class ("wd_a7", "a", sizes, 1, true);
  direct_class ("wd_a8", "a", sizes, 2, true);
  direct_class ("wd_a8", "b", sizes, 1, true);
  direct_class ("wd_a9", "a", sizes, 2, true);
  direct_class ("wd_a9", "g", sizes, 1, true);
  direct_class ("wd_many", "v3", sizes, 1, true);
  direct_class ("wd_many", "v0", sizes, 1, true);
  direct_class ("wd_fixed", "v1", sizes, 2, true);
  direct_class ("wd_bb33", "v", sizes, 2, true);
  direct_class ("wd_incr", "a", sizes, 1, true, "cnt");
  direct_class ("wd_incr", nullptr, sizes, 0, false, "scnt");
  direct_class ("wd_incr", nullptr, sizes, 0, false, "bcnt");
  direct_class ("wd_old", "a", sizes, 4, true);
  direct_class ("db_serial", nullptr, sizes, 0, false);
  direct_class ("wd_varonly", "a", sizes, 2, true);
  direct_class ("wd_fixonly", nullptr, sizes, 0, true, "s");
  direct_class ("wd_noattr", nullptr, sizes, 0, true);
  direct_class ("wd_types", "nch", sizes, 3, true);
  db_abort_transaction ();
  out ("ABORT\n");
}

int
main (int argc, char **argv)
{
  ::testing::InitGoogleTest (&argc, argv);
  const char *path = getenv ("WRITEDIFF_OUT");
  g_out = fopen (path != nullptr ? path : "writediff_dump.txt", "w");
  if (g_out == nullptr)
    {
      perror ("fopen");
      return 2;
    }
  ::testing::AddGlobalTestEnvironment (new SqlServerEnv ());
  const int rc = RUN_ALL_TESTS ();
  fclose (g_out);
  return rc;
}
