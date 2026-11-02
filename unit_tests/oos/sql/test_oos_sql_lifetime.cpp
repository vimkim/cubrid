/*
 *
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
 * test_oos_sql_lifetime.cpp - Adversarial lifetime checks of the OOS read paths (PR 8299 review, adversary 2).
 *
 * Every DB_VALUE decoded from an OOS value must own its bytes: it must survive the release of the record
 * copy, of the grouped payload buffers, of the scalar scratch or heap buffer, and of a pending owner.
 * Run under the asan_debug_gcc preset with use_system_malloc=yes so CUBRID private allocations are
 * individually visible to AddressSanitizer.
 */

#include "test_oos_sql_common.hpp"

#include "db_json.hpp"
#include "db_set_function.h"
#include "heap_oos.hpp"
#include "heap_oos_value_ref.hpp"
#include "heap_pending_record.hpp"
#include "language_support.h"
#include "log_impl.h"
#include "object_primitive.h"
#include "object_representation.h"
#include "scope_exit.hpp"
#include "work_space.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/lsan_interface.h>
#endif

// XXX: SHOULD BE THE LAST INCLUDE HEADER
#include "memory_wrapper.hpp"

void bridge_oos_debug_counters_reset ();
oos_debug_counters bridge_oos_debug_counters_get ();

namespace
{
  const unsigned char POISON = 0xEE;

  // ------------------------------------------------------------------------------------------------
  // Value fingerprints: a byte-exact, type-tagged string that only reads through the DB_VALUE.
  // ------------------------------------------------------------------------------------------------
  std::string fp (const DB_VALUE *v);

  std::string
  fp_collection (const DB_VALUE *v)
  {
    DB_COLLECTION *col = db_get_set (v);
    if (col == nullptr)
      {
	return "C<null>";
      }
    int n = db_col_size (col);
    std::vector<std::string> parts;
    for (int i = 0; i < n; ++i)
      {
	DB_VALUE e;
	db_make_null (&e);
	if (db_col_get (col, i, &e) != NO_ERROR)
	  {
	    parts.push_back ("<get-error>");
	    continue;
	  }
	parts.push_back (fp (&e));
	db_value_clear (&e);
      }
    if (DB_VALUE_TYPE (v) != DB_TYPE_SEQUENCE)
      {
	std::sort (parts.begin (), parts.end ());
      }
    std::string out = "C" + std::to_string ((int) DB_VALUE_TYPE (v)) + "[";
    for (const std::string &p : parts)
      {
	out += p + "|";
      }
    return out + "]";
  }

  std::string
  fp (const DB_VALUE *v)
  {
    if (v == nullptr)
      {
	return "<nullptr>";
      }
    if (DB_IS_NULL (v))
      {
	return "NULL";
      }
    switch (DB_VALUE_TYPE (v))
      {
      case DB_TYPE_INTEGER:
	return "I:" + std::to_string (db_get_int (v));
      case DB_TYPE_CHAR:
      case DB_TYPE_VARCHAR:
      {
	const char *s = db_get_string (v);
	int n = db_get_string_size (v);
	return "S:" + (s == nullptr ? std::string ("<null>") : std::string (s, n));
      }
      case DB_TYPE_BIT:
      case DB_TYPE_VARBIT:
      {
	int bits = 0;
	const char *s = db_get_bit (v, &bits);
	return "B" + std::to_string (bits) + ":" + (s == nullptr ? std::string ("<null>")
	       : std::string (s, (bits + 7) / 8));
      }
      case DB_TYPE_SET:
      case DB_TYPE_MULTISET:
      case DB_TYPE_SEQUENCE:
	return fp_collection (v);
      case DB_TYPE_JSON:
      {
	JSON_DOC *doc = db_get_json_document (v);
	if (doc == nullptr)
	  {
	    return "J<null>";
	  }
	char *body = db_json_get_json_body_from_document (*doc);
	std::string out = "J:" + std::string (body == nullptr ? "<null>" : body);
	if (body != nullptr)
	  {
	    db_private_free_and_init (NULL, body);
	  }
	return out;
      }
      case DB_TYPE_BLOB:
      case DB_TYPE_CLOB:
      {
	DB_ELO *e = db_get_elo (v);
	if (e == nullptr)
	  {
	    return "L<null>";
	  }
	return "L" + std::to_string ((int) DB_VALUE_TYPE (v)) + ":" + std::to_string ((long long) e->size) + ":"
	       + (e->locator ? e->locator : "<no locator>") + ":" + (e->meta_data ? e->meta_data : "<no meta>");
      }
      default:
	return "T" + std::to_string ((int) DB_VALUE_TYPE (v));
      }
  }

  // ------------------------------------------------------------------------------------------------
  // Expected values of the typed table.
  // ------------------------------------------------------------------------------------------------
  std::string exp_v (int id)
  {
    return std::string (2000 + 17 * id, (char) ('a' + id));
  }
  std::string exp_c (int id)
  {
    return std::string (1500 + id, 'c');
  }
  std::string exp_b (int id)
  {
    return std::string (4000 + 13 * id, (char) (0x30 + id));
  }
  std::vector<std::string> exp_s (int id)
  {
    std::vector<std::string> out;
    for (int k = 0; k < 10; ++k)
      {
	out.push_back ("s" + std::to_string (id) + "-" + std::to_string (k) + "-" + std::string (300, 'x'));
      }
    return out;
  }
  std::vector<std::string> exp_m (int id)
  {
    std::vector<std::string> out;
    for (int k = 0; k < 9; ++k)
      {
	out.push_back ("m" + std::to_string (id) + "-" + std::to_string (k % 3) + "-" + std::string (300, 'y'));
      }
    return out;
  }
  std::vector<std::string> exp_q (int id)
  {
    std::vector<std::string> out;
    for (int k = 0; k < 10; ++k)
      {
	out.push_back ("q" + std::to_string (id) + "-" + std::to_string (9 - k) + "-" + std::string (300, 'z'));
      }
    return out;
  }
  std::string exp_j_text (int id)
  {
    return "{\"id\":" + std::to_string (id) + ",\"pad\":\"" + std::string (800, 'j') + "\"}";
  }

  std::string hex_of (const std::string &bytes)
  {
    static const char *digits = "0123456789ABCDEF";
    std::string out;
    out.reserve (bytes.size () * 2);
    for (unsigned char c : bytes)
      {
	out.push_back (digits[c >> 4]);
	out.push_back (digits[c & 15]);
      }
    return out;
  }

  std::string sql_collection (const std::vector<std::string> &elems)
  {
    std::string out = "{";
    for (size_t i = 0; i < elems.size (); ++i)
      {
	out += (i ? ", '" : "'") + elems[i] + "'";
      }
    return out + "}";
  }

  std::string fp_strings (int type, std::vector<std::string> elems, bool keep_order)
  {
    std::vector<std::string> parts;
    for (const std::string &e : elems)
      {
	parts.push_back ("S:" + e);
      }
    if (!keep_order)
      {
	std::sort (parts.begin (), parts.end ());
      }
    if (type == DB_TYPE_SET)
      {
	parts.erase (std::unique (parts.begin (), parts.end ()), parts.end ());
      }
    std::string out = "C" + std::to_string (type) + "[";
    for (const std::string &p : parts)
      {
	out += p + "|";
      }
    return out + "]";
  }

  // JSON and LOB fingerprints are taken from an independent path: a client-side SELECT of one column.
  std::map<std::string, std::map<int, std::string>> g_ref_fp;

  std::string expected_fp (const std::string &col, int id)
  {
    if (col == "id")
      {
	return "I:" + std::to_string (id);
      }
    if (col == "v")
      {
	return "S:" + exp_v (id);
      }
    if (col == "c" || col == "l" || col == "j")
      {
	return g_ref_fp[col].count (id) ? g_ref_fp[col][id] : std::string ("<no reference for ") + col + ">";
      }
    if (col == "b")
      {
	std::string b = exp_b (id);
	return "B" + std::to_string (b.size () * 8) + ":" + b;
      }
    if (col == "s")
      {
	return fp_strings (DB_TYPE_SET, exp_s (id), false);
      }
    if (col == "m")
      {
	return fp_strings (DB_TYPE_MULTISET, exp_m (id), false);
      }
    if (col == "q")
      {
	return fp_strings (DB_TYPE_SEQUENCE, exp_q (id), true);
      }
    return "<unknown column " + col + ">";
  }

  // ------------------------------------------------------------------------------------------------
  // Heap access helpers.
  // ------------------------------------------------------------------------------------------------
  struct class_handle
  {
    DB_OBJECT *cls = nullptr;
    OID class_oid = OID_INITIALIZER;
    HFID hfid = HFID_INITIALIZER;
  };

  bool open_class (const char *name, class_handle &h)
  {
    THREAD_ENTRY *thread = thread_get_thread_entry_info ();
    h.cls = db_find_class (name);
    if (h.cls == nullptr)
      {
	return false;
      }
    h.class_oid = *WS_OID (h.cls);
    return heap_get_class_hfid (thread, &h.class_oid, &h.hfid, nullptr) == NO_ERROR;
  }

  ATTR_ID attr_id (const class_handle &h, const char *name)
  {
    DB_ATTRIBUTE *a = db_get_attribute (h.cls, name);
    return a == nullptr ? -1 : db_attribute_id (a);
  }

  int fetch_bytes (const class_handle &h, const OID &oid, std::vector<char> &out)
  {
    THREAD_ENTRY *thread = thread_get_thread_entry_info ();
    HEAP_SCANCACHE scan;
    int err = heap_scancache_start (thread, &scan, &h.hfid, &h.class_oid, true, logtb_get_mvcc_snapshot (thread));
    if (err != NO_ERROR)
      {
	return err;
      }
    RECDES record = RECDES_INITIALIZER;
    OID oid_copy = oid;
    OID class_copy = h.class_oid;
    const SCAN_CODE status = heap_get_visible_version (thread, &oid_copy, &class_copy, &record, &scan, COPY,
			     NULL_CHN, HEAP_RECDES_DONT_CONSUME_RAW_BYTES);
    if (status == S_SUCCESS)
      {
	out.assign (record.data, record.data + record.length);
      }
    heap_scancache_end (thread, &scan);
    return status == S_SUCCESS ? NO_ERROR : ER_FAILED;
  }

  struct lt_row
  {
    OID oid;
    int id;
    std::vector<char> bytes;
  };

  // All rows of the class, with their id taken from a separate scalar read of the INT column.
  bool collect_rows (const class_handle &h, std::vector<lt_row> &rows)
  {
    THREAD_ENTRY *thread = thread_get_thread_entry_info ();
    DB_OBJLIST *objects = db_fetch_all_objects (h.cls, DB_FETCH_READ);
    scope_exit free_objects ([&] ()
    {
      db_objlist_free (objects);
    });
    ATTR_ID id_attr = attr_id (h, "id");
    HEAP_CACHE_ATTRINFO attrs;
    if (heap_attrinfo_start (thread, &h.class_oid, 1, &id_attr, &attrs) != NO_ERROR)
      {
	return false;
      }
    scope_exit end_attrs ([&] ()
    {
      heap_attrinfo_end (thread, &attrs);
    });
    for (DB_OBJLIST *item = objects; item != nullptr; item = item->next)
      {
	lt_row r;
	r.oid = *WS_OID (item->op);
	if (fetch_bytes (h, r.oid, r.bytes) != NO_ERROR)
	  {
	    return false;
	  }
	RECDES rec = { (int) r.bytes.size (), (int) r.bytes.size (), REC_HOME, r.bytes.data () };
	if (heap_attrinfo_read_dbvalues (thread, &r.oid, &rec, &attrs) != NO_ERROR)
	  {
	    return false;
	  }
	r.id = db_get_int (heap_attrinfo_access (id_attr, &attrs));
	rows.push_back (std::move (r));
      }
    std::sort (rows.begin (), rows.end (), [] (const lt_row &a, const lt_row &b)
    {
      return a.id < b.id;
    });
    return true;
  }

  // Raw OOS-ness of variable attribute `location` in a record, from the record's own offset table.
  bool record_attr_is_oos (const RECDES &rec, int location)
  {
    int entry = 0, next = 0;
    if (heap_recdes_get_var_offset_entry (&rec, location, &entry) != NO_ERROR
	|| heap_recdes_get_var_offset_entry (&rec, location + 1, &next) != NO_ERROR)
      {
	return false;
      }
    return OR_IS_OOS (entry) && OR_GET_VAR_OFFSET (entry) != OR_GET_VAR_OFFSET (next);
  }

  int location_of (HEAP_CACHE_ATTRINFO &attrs, ATTR_ID id)
  {
    for (int i = 0; i < attrs.num_values; ++i)
      {
	if (attrs.values[i].attrid == id)
	  {
	    OR_ATTRIBUTE *a = attrs.values[i].read_attrepr != nullptr ? attrs.values[i].read_attrepr
			      : attrs.values[i].last_attrepr;
	    return a->is_fixed ? -1 : a->location;
	  }
      }
    return -1;
  }

  // Read `bytes` from a private heap copy, poison and release that copy, and return the read status.
  // Values that borrow the record (inline variable data read without COPY) must not be touched after
  // this returns; OOS-backed values must stay intact.
  int read_then_drop_record (const OID &oid, const std::vector<char> &bytes, HEAP_CACHE_ATTRINFO &attrs,
			     const heap_pending_record *pending = nullptr)
  {
    THREAD_ENTRY *thread = thread_get_thread_entry_info ();
    char *copy = (char *) malloc (bytes.size ());
    std::memcpy (copy, bytes.data (), bytes.size ());
    RECDES rec = { (int) bytes.size (), (int) bytes.size (), REC_HOME, copy };
    const int err = heap_attrinfo_read_dbvalues (thread, &oid, &rec, &attrs, pending);
    std::memset (copy, POISON, bytes.size ());
    free (copy);
    return err;
  }

  // Overwrite the OOS inline stub fields of variable attribute `location` in a record byte image.
  enum class stub_damage { zero_length, longer_length, identity_stamp, slot };

  bool damage_stub (std::vector<char> &bytes, int location, stub_damage how)
  {
    RECDES rec = { (int) bytes.size (), (int) bytes.size (), REC_HOME, bytes.data () };
    char *stub = nullptr;
    if (heap_recdes_get_oos_inline_stub (&rec, location, &stub) != NO_ERROR)
      {
	return false;
      }
    OID oid;
    DB_BIGINT length = 0, stamp = 0;
    OR_GET_OID (stub, &oid);
    OR_GET_BIGINT (stub + OR_OID_SIZE, &length);
    OR_GET_BIGINT (stub + OR_OID_SIZE + OR_BIGINT_SIZE, &stamp);
    switch (how)
      {
      case stub_damage::zero_length:
	length = 0;
	break;
      case stub_damage::longer_length:
	length += 4;
	break;
      case stub_damage::identity_stamp:
	stamp ^= 1;
	break;
      case stub_damage::slot:
	oid.slotid += 40;
	break;
      }
    OR_BUF buf;
    or_init (&buf, stub, OR_OOS_INLINE_SIZE);
    or_put_oid (&buf, &oid);
    or_put_bigint (&buf, length);
    or_put_bigint (&buf, stamp);
    return true;
  }

  const char *damage_name (stub_damage how)
  {
    switch (how)
      {
      case stub_damage::zero_length:
	return "zero_length";
      case stub_damage::longer_length:
	return "longer_length";
      case stub_damage::identity_stamp:
	return "identity_stamp";
      case stub_damage::slot:
	return "slot";
      }
    return "?";
  }

  // ------------------------------------------------------------------------------------------------
  // Table builders.
  // ------------------------------------------------------------------------------------------------
  const char *TYPED_COLS[] = { "v", "c", "b", "s", "m", "q", "j", "l" };

  void create_typed_table ()
  {
    ASSERT_GE (exec_sql ("CREATE TABLE t_lt_typed (id INT, "
			 "v VARCHAR STORAGE FORCE_OUTLINE, "
			 "c CLOB STORAGE FORCE_OUTLINE, "
			 "b BIT VARYING STORAGE FORCE_OUTLINE, "
			 "s SET(VARCHAR(400)) STORAGE FORCE_OUTLINE, "
			 "m MULTISET(VARCHAR(400)) STORAGE FORCE_OUTLINE, "
			 "q SEQUENCE(VARCHAR(400)) STORAGE FORCE_OUTLINE, "
			 "j JSON STORAGE FORCE_OUTLINE, "
			 "l BLOB STORAGE FORCE_OUTLINE)"), 0);
    for (int id = 1; id <= 3; ++id)
      {
	std::string sql = "INSERT INTO t_lt_typed VALUES (" + std::to_string (id) + ", '" + exp_v (id)
			  + "', CHAR_TO_CLOB ('" + exp_c (id) + "'), X'" + hex_of (exp_b (id)) + "', "
			  + sql_collection (exp_s (id)) + ", " + sql_collection (exp_m (id)) + ", "
			  + sql_collection (exp_q (id)) + ", '" + exp_j_text (id) + "', BIT_TO_BLOB (X'"
			  + hex_of (std::string (700 + id, (char) (0x60 + id))) + "'))";
	ASSERT_GE (exec_sql (sql.c_str ()), 0) << "insert id " << id;
      }
    ASSERT_EQ (db_commit_transaction (), NO_ERROR);

    // Reference fingerprints from an independent path (one-column SELECT through the client).
    for (const char *col :
	 { "j", "c", "l"
	 })
      {
	for (int id = 1; id <= 3; ++id)
	  {
	    DB_QUERY_RESULT *result = nullptr;
	    std::string sql = std::string ("SELECT ") + col + " FROM t_lt_typed WHERE id = " + std::to_string (id);
	    ASSERT_GE (exec_sql_with_result (sql.c_str (), &result), 0);
	    ASSERT_EQ (db_query_first_tuple (result), DB_CURSOR_SUCCESS);
	    DB_VALUE val;
	    db_make_null (&val);
	    ASSERT_EQ (db_query_get_tuple_value (result, 0, &val), NO_ERROR);
	    g_ref_fp[col][id] = fp (&val);
	    db_value_clear (&val);
	    db_query_end (result);
	    printf ("[lifetime] reference %s id %d: %s\n", col, id, g_ref_fp[col][id].substr (0, 140).c_str ());
	  }
	EXPECT_NE (g_ref_fp["j"][1].find (std::string (800, 'j')), std::string::npos);
      }
  }
}

class OosSqlLifetime : public ::testing::Test
{
  protected:
    void SetUp () override
    {
      drop ();
    }
    void TearDown () override
    {
      drop ();
    }
    static void drop ()
    {
      exec_sql ("DROP TABLE IF EXISTS t_lt_typed");
      exec_sql ("DROP TABLE IF EXISTS t_lt_many");
      exec_sql ("DROP TABLE IF EXISTS t_lt_pend");
      exec_sql ("DROP TABLE IF EXISTS t_lt_idx");
      exec_sql ("DROP TABLE IF EXISTS t_lt_scr");
      db_commit_transaction ();
    }
};

// ==================================================================================================
// L1: grouped and scalar reads of every OOS-capable variable type. After the read returns, the record
// copy is poisoned and freed, then every OOS-backed value is checked, cloned, and checked again after
// the next row overwrote the slots and after the attribute cache ended.
// ==================================================================================================
TEST_F (OosSqlLifetime, TypedValuesOutliveRecordPayloadAndNextRow)
{
  create_typed_table ();
  if (HasFatalFailure ())
    {
      return;
    }
  class_handle h;
  ASSERT_TRUE (open_class ("t_lt_typed", h));
  std::vector<lt_row> rows;
  ASSERT_TRUE (collect_rows (h, rows));
  ASSERT_EQ (rows.size (), 3u);

  // Every value must actually be OOS for the grouped path to apply.
  {
    THREAD_ENTRY *thread = thread_get_thread_entry_info ();
    HEAP_CACHE_ATTRINFO all;
    ASSERT_EQ (heap_attrinfo_start (thread, &h.class_oid, -1, nullptr, &all), NO_ERROR);
    for (const lt_row &r : rows)
      {
	RECDES rec = { (int) r.bytes.size (), (int) r.bytes.size (), REC_HOME, (char *) r.bytes.data () };
	for (const char *col : TYPED_COLS)
	  {
	    EXPECT_TRUE (record_attr_is_oos (rec, location_of (all, attr_id (h, col)))) << col << " id " << r.id;
	  }
      }
    heap_attrinfo_end (thread, &all);
  }

  const std::vector<std::vector<std::string>> subsets =
  {
    { "id", "v", "c", "b", "s", "m", "q", "j", "l" },
    { "l", "q", "j", "v", "c", "b", "s", "m", "id" },
    { "c", "l" }, { "l" },
    { "v", "b" }, { "s", "m", "q" }, { "j", "s" }, { "q", "j", "id" }, { "c", "v" },
    { "s" }, { "m" }, { "q" }, { "j" }, { "c" }, { "id", "q" }, { "b", "id" }
  };

  THREAD_ENTRY *thread = thread_get_thread_entry_info ();
  for (const auto &subset : subsets)
    {
      std::vector<ATTR_ID> ids;
      int oos_requested = 0;
      std::string label;
      for (const std::string &col : subset)
	{
	  ids.push_back (attr_id (h, col.c_str ()));
	  oos_requested += col != "id";
	  label += col + ",";
	}
      HEAP_CACHE_ATTRINFO attrs;
      ASSERT_EQ (heap_attrinfo_start (thread, &h.class_oid, (int) ids.size (), ids.data (), &attrs), NO_ERROR);
      std::vector<std::vector<DB_VALUE>> clones;
      for (const lt_row &r : rows)
	{
	  // The OID-less entry point takes the same grouped path; its values must also own their bytes.
	  {
	    std::vector<char> copy = r.bytes;
	    RECDES rec = { (int) copy.size (), (int) copy.size (), REC_HOME, copy.data () };
	    ASSERT_EQ (heap_attrinfo_read_dbvalues_without_oid (thread, &rec, &attrs), NO_ERROR) << label;
	    std::fill (copy.begin (), copy.end (), (char) POISON);
	    for (size_t i = 0; i < subset.size (); ++i)
	      {
		EXPECT_EQ (fp (heap_attrinfo_access (ids[i], &attrs)), expected_fp (subset[i], r.id))
		    << label << " without_oid col " << subset[i];
	      }
	  }
	  bridge_oos_debug_counters_reset ();
	  ASSERT_EQ (read_then_drop_record (r.oid, r.bytes, attrs), NO_ERROR) << label;
	  const oos_debug_counters counters = bridge_oos_debug_counters_get ();
	  EXPECT_EQ (counters.read_many_calls, oos_requested >= 2 ? 1ULL : 0ULL) << label;
	  EXPECT_EQ (counters.read_many_requests, oos_requested >= 2 ? (unsigned long long) oos_requested : 0ULL)
	      << label;

	  std::vector<DB_VALUE> kept (subset.size ());
	  for (size_t i = 0; i < subset.size (); ++i)
	    {
	      const DB_VALUE *value = heap_attrinfo_access (ids[i], &attrs);
	      EXPECT_EQ (fp (value), expected_fp (subset[i], r.id)) << label << " col " << subset[i] << " id " << r.id;
	      db_make_null (&kept[i]);
	      ASSERT_EQ (pr_clone_value (value, &kept[i]), NO_ERROR);
	    }
	  clones.push_back (std::move (kept));
	}
      // Slots now hold the last row; the clones of every row stay intact.
      heap_attrinfo_end (thread, &attrs);
      for (size_t row_index = 0; row_index < clones.size (); ++row_index)
	{
	  for (size_t i = 0; i < subset.size (); ++i)
	    {
	      EXPECT_EQ (fp (&clones[row_index][i]), expected_fp (subset[i], rows[row_index].id))
		  << label << " clone of col " << subset[i];
	      pr_clear_value (&clones[row_index][i]);
	    }
	}
    }
}

// ==================================================================================================
// L2: the same values through SQL: predicates, ORDER BY and DISTINCT on OOS columns, values compared
// after the query result was closed, then after a server-side UPDATE and an INSERT ... SELECT that both
// rebuild records from grouped reads.
// ==================================================================================================
namespace
{
  // Run `sql`, keep a deep copy of every value, close the result, and return fingerprints per row.
  std::vector<std::vector<std::string>> query_rows_after_end (const std::string &sql, int columns)
  {
    std::vector<std::vector<DB_VALUE>> kept;
    DB_QUERY_RESULT *result = nullptr;
    int rc = exec_sql_with_result (sql.c_str (), &result);
    EXPECT_GE (rc, 0) << sql.substr (0, 200) << " : " << db_error_string (3);
    if (rc < 0 || result == nullptr)
      {
	return {};
      }
    for (int pos = db_query_first_tuple (result); pos == DB_CURSOR_SUCCESS; pos = db_query_next_tuple (result))
      {
	std::vector<DB_VALUE> values (columns);
	for (int c = 0; c < columns; ++c)
	  {
	    db_make_null (&values[c]);
	    EXPECT_EQ (db_query_get_tuple_value (result, c, &values[c]), NO_ERROR);
	  }
	kept.push_back (std::move (values));
      }
    db_query_end (result);
    std::vector<std::vector<std::string>> out;
    for (auto &values : kept)
      {
	std::vector<std::string> fps;
	for (DB_VALUE &v : values)
	  {
	    fps.push_back (fp (&v));
	    db_value_clear (&v);
	  }
	out.push_back (std::move (fps));
      }
    return out;
  }
}

TEST_F (OosSqlLifetime, SqlValuesOutliveScanUpdateAndCopy)
{
  create_typed_table ();
  if (HasFatalFailure ())
    {
      return;
    }
  const std::vector<std::string> cols = { "v", "c", "b", "s", "m", "q", "j", "l" };

  auto check_all = [&] (int id_offset, const char *where)
  {
    auto out = query_rows_after_end (std::string ("SELECT id, v, c, b, s, m, q, j, l FROM t_lt_typed ") + where
				     + " ORDER BY id", 9);
    EXPECT_EQ (out.size (), 3u) << where;
    for (size_t r = 0; r < out.size (); ++r)
      {
	int id = (int) r + 1;
	EXPECT_EQ (out[r][0], "I:" + std::to_string (id + id_offset));
	for (size_t c = 0; c < cols.size (); ++c)
	  {
	    EXPECT_EQ (out[r][c + 1], expected_fp (cols[c], id)) << where << " col " << cols[c] << " row " << id;
	  }
      }
  };

  check_all (0, "");
  check_all (0, "WHERE v IS NOT NULL AND b IS NOT NULL AND j IS NOT NULL");
  check_all (0,
	     "WHERE LENGTH (v) > 100 AND NOT (SET {'nope'} SUBSETEQ s) AND q IS NOT NULL AND m IS NOT NULL AND JSON_LENGTH (j) = 2 AND BIT_LENGTH (b) > 8");

  // OOS columns used as sort keys and as predicate operands.
  {
    auto out = query_rows_after_end ("SELECT id, q, j FROM t_lt_typed ORDER BY v DESC, b", 3);
    ASSERT_EQ (out.size (), 3u);
    for (size_t r = 0; r < 3; ++r)
      {
	int id = 3 - (int) r;
	EXPECT_EQ (out[r][0], "I:" + std::to_string (id));
	EXPECT_EQ (out[r][1], expected_fp ("q", id));
	EXPECT_EQ (out[r][2], expected_fp ("j", id));
      }
    out = query_rows_after_end ("SELECT DISTINCT s, m FROM t_lt_typed WHERE v = '" + exp_v (2) + "'", 2);
    ASSERT_EQ (out.size (), 1u);
    EXPECT_EQ (out[0][0], expected_fp ("s", 2));
    EXPECT_EQ (out[0][1], expected_fp ("m", 2));
  }

  // Server-side UPDATE re-encodes every row from a grouped read of its OOS columns.
  ASSERT_GE (exec_sql ("UPDATE t_lt_typed SET id = id + 10"), 0);
  check_all (10, "");
  ASSERT_EQ (db_commit_transaction (), NO_ERROR);
  check_all (10, "");
  ASSERT_GE (exec_sql ("UPDATE t_lt_typed SET id = id - 10 WHERE id > 10"), 0);
  ASSERT_EQ (db_commit_transaction (), NO_ERROR);
  check_all (0, "");

  // INSERT ... SELECT copies OOS values row to row.
  ASSERT_GE (exec_sql ("INSERT INTO t_lt_typed SELECT id + 100, v, c, b, s, m, q, j, l FROM t_lt_typed"), 0);
  ASSERT_EQ (db_commit_transaction (), NO_ERROR);
  {
    auto out = query_rows_after_end ("SELECT id, v, c, b, s, m, q, j FROM t_lt_typed WHERE id > 100 ORDER BY id", 8);
    ASSERT_EQ (out.size (), 3u);
    for (size_t r = 0; r < 3; ++r)
      {
	int id = (int) r + 1;
	EXPECT_EQ (out[r][0], "I:" + std::to_string (id + 100));
	for (size_t c = 0; c < cols.size (); ++c)
	  {
	    if (cols[c] != "c" && cols[c] != "l")
	      {
		EXPECT_EQ (out[r][c + 1], expected_fp (cols[c], id)) << "copied col " << cols[c];
	      }
	  }
	// A copied LOB gets its own locator; its content must match.
	int same = -1;
	std::string sql = "SELECT COUNT(*) FROM t_lt_typed WHERE id = " + std::to_string (id + 100)
			  + " AND CLOB_TO_CHAR (c) = '" + exp_c (id) + "' AND BLOB_TO_BIT (l) = X'"
			  + hex_of (std::string (700 + id, (char) (0x60 + id))) + "'";
	ASSERT_EQ (fetch_single_int (sql.c_str (), &same), NO_ERROR);
	EXPECT_EQ (same, 1) << "copied LOB content id " << id;
      }
  }
}

// ==================================================================================================
// L3: requested OOS counts 0, 1, 2, 7, 8, 9 and 10 around the 8-entry stack array of oos_read_many(),
// with heads that share a page, heads with continuations, and a NULL column inside the request.
// ==================================================================================================
namespace
{
  const int MANY = 10;

  int many_size (int id, int col)
  {
    switch (id)
      {
      case 1:
	return 100 + col;		// every head on one page
      case 2:
	return (col % 2) ? 20000 + col : 100 + col;	// continuations interleaved
      case 3:
	return 3000 + 7 * col;
      default:
	return 64 + col;
      }
  }

  std::string many_bytes (int id, int col)
  {
    return std::string (many_size (id, col), (char) (0x40 + id * 16 + col));
  }

  bool many_is_null (int id, int col)
  {
    return id == 3 && col == 4;
  }

  std::string many_expected (int id, int col)
  {
    if (many_is_null (id, col))
      {
	return "NULL";
      }
    std::string b = many_bytes (id, col);
    return "B" + std::to_string (b.size () * 8) + ":" + b;
  }

  void create_many_table ()
  {
    std::string ddl = "CREATE TABLE t_lt_many (id INT";
    for (int c = 0; c < MANY; ++c)
      {
	ddl += ", c" + std::to_string (c) + " BIT VARYING STORAGE FORCE_OUTLINE";
      }
    ddl += ", tail VARCHAR(20))";
    ASSERT_GE (exec_sql (ddl.c_str ()), 0);
    for (int id = 1; id <= 3; ++id)
      {
	std::string sql = "INSERT INTO t_lt_many VALUES (" + std::to_string (id);
	for (int c = 0; c < MANY; ++c)
	  {
	    sql += many_is_null (id, c) ? std::string (", NULL") : ", X'" + hex_of (many_bytes (id, c)) + "'";
	  }
	sql += ", 'tail" + std::to_string (id) + "')";
	ASSERT_GE (exec_sql (sql.c_str ()), 0);
      }
    ASSERT_EQ (db_commit_transaction (), NO_ERROR);
  }
}

TEST_F (OosSqlLifetime, RequestCountsAroundReadManyStackArray)
{
  create_many_table ();
  if (HasFatalFailure ())
    {
      return;
    }
  class_handle h;
  ASSERT_TRUE (open_class ("t_lt_many", h));
  std::vector<lt_row> rows;
  ASSERT_TRUE (collect_rows (h, rows));
  ASSERT_EQ (rows.size (), 3u);
  THREAD_ENTRY *thread = thread_get_thread_entry_info ();

  for (int k :
       {
	       0, 1, 2, 7, 8, 9, 10
       })
    {
      for (bool reversed :
	   {
		   false, true
	   })
	{
	  std::vector<ATTR_ID> ids;
	  std::vector<int> cols;
	  for (int c = 0; c < k; ++c)
	    {
	      cols.push_back (reversed ? MANY - 1 - c : c);
	    }
	  for (int c : cols)
	    {
	      ids.push_back (attr_id (h, ("c" + std::to_string (c)).c_str ()));
	    }
	  // id is fixed and tail is inline: neither counts as an OOS request.
	  ids.push_back (attr_id (h, "id"));
	  HEAP_CACHE_ATTRINFO attrs;
	  ASSERT_EQ (heap_attrinfo_start (thread, &h.class_oid, (int) ids.size (), ids.data (), &attrs), NO_ERROR);
	  for (const lt_row &r : rows)
	    {
	      int oos = 0;
	      for (int c : cols)
		{
		  oos += !many_is_null (r.id, c);
		}
	      bridge_oos_debug_counters_reset ();
	      ASSERT_EQ (read_then_drop_record (r.oid, r.bytes, attrs), NO_ERROR) << "k=" << k;
	      const oos_debug_counters counters = bridge_oos_debug_counters_get ();
	      EXPECT_EQ (counters.read_many_calls, oos >= 2 ? 1ULL : 0ULL) << "k=" << k << " id " << r.id;
	      EXPECT_EQ (counters.read_many_requests, oos >= 2 ? (unsigned long long) oos : 0ULL)
		  << "k=" << k << " id " << r.id;
	      for (size_t i = 0; i < cols.size (); ++i)
		{
		  EXPECT_EQ (fp (heap_attrinfo_access (ids[i], &attrs)), many_expected (r.id, cols[i]))
		      << "k=" << k << " col " << cols[i] << " id " << r.id << (reversed ? " reversed" : "");
		}
	      EXPECT_EQ (db_get_int (heap_attrinfo_access (ids.back (), &attrs)), r.id);
	    }
	  heap_attrinfo_end (thread, &attrs);
	}
    }

  // requested_num_attrs == 0 leaves an empty cache that a read must reject or ignore without touching OOS.
  {
    HEAP_CACHE_ATTRINFO attrs;
    ASSERT_EQ (heap_attrinfo_start (thread, &h.class_oid, 0, nullptr, &attrs), NO_ERROR);
    bridge_oos_debug_counters_reset ();
    const int err = read_then_drop_record (rows[0].oid, rows[0].bytes, attrs);
    printf ("[lifetime] read with requested_num_attrs=0 returned %d, read_many_calls=%llu\n", err,
	    bridge_oos_debug_counters_get ().read_many_calls);
    EXPECT_EQ (bridge_oos_debug_counters_get ().read_many_calls, 0ULL);
    er_clear ();
    heap_attrinfo_end (thread, &attrs);
  }

  // All attributes (-1): 10 OOS requests plus fixed and inline values.
  {
    HEAP_CACHE_ATTRINFO attrs;
    ASSERT_EQ (heap_attrinfo_start (thread, &h.class_oid, -1, nullptr, &attrs), NO_ERROR);
    for (const lt_row &r : rows)
      {
	std::vector<char> copy = r.bytes;
	RECDES rec = { (int) copy.size (), (int) copy.size (), REC_HOME, copy.data () };
	ASSERT_EQ (heap_attrinfo_read_dbvalues (thread, &r.oid, &rec, &attrs), NO_ERROR);
	EXPECT_EQ (fp (heap_attrinfo_access (attr_id (h, "tail"), &attrs)), "S:tail" + std::to_string (r.id));
	std::fill (copy.begin (), copy.end (), (char) POISON);
	for (int c = 0; c < MANY; ++c)
	  {
	    EXPECT_EQ (fp (heap_attrinfo_access (attr_id (h, ("c" + std::to_string (c)).c_str ()), &attrs)),
		       many_expected (r.id, c)) << "all col " << c;
	  }
      }
    heap_attrinfo_end (thread, &attrs);
  }
}

// ==================================================================================================
// L4: failures in the grouped prefetch and in the scalar Resolve, at the first, a middle, the 9th and
// the last request. After the failed read the cache must stay usable: every slot readable, a later good
// read correct, and heap_attrinfo_end clean. LeakSanitizer reports any payload left allocated.
// ==================================================================================================
TEST_F (OosSqlLifetime, FailedReadsReleasePayloadsAndKeepSlotsUsable)
{
  create_many_table ();
  if (HasFatalFailure ())
    {
      return;
    }
  class_handle h;
  ASSERT_TRUE (open_class ("t_lt_many", h));
  std::vector<lt_row> rows;
  ASSERT_TRUE (collect_rows (h, rows));
  ASSERT_EQ (rows.size (), 3u);
  THREAD_ENTRY *thread = thread_get_thread_entry_info ();

  struct plan
  {
    int requested;		// c0 .. c(requested-1)
    int damaged;		// column whose stub is damaged
  };
  const plan plans[] = { { 10, 0 }, { 10, 4 }, { 10, 8 }, { 10, 9 }, { 9, 8 }, { 9, 0 }, { 8, 7 }, { 2, 1 }, { 1, 0 } };
  const stub_damage damages[] = { stub_damage::zero_length, stub_damage::longer_length, stub_damage::identity_stamp,
				  stub_damage::slot
				};

  for (const lt_row &r : rows)
    {
      if (r.id == 3)
	{
	  continue;		// row 3 has a NULL column; rows 1 and 2 cover shared heads and continuations
	}
      for (const plan &p : plans)
	{
	  for (stub_damage how : damages)
	    {
	      std::string label = "id " + std::to_string (r.id) + " requested " + std::to_string (p.requested)
				  + " damaged c" + std::to_string (p.damaged) + " " + damage_name (how);
	      std::vector<ATTR_ID> ids;
	      for (int c = 0; c < p.requested; ++c)
		{
		  ids.push_back (attr_id (h, ("c" + std::to_string (c)).c_str ()));
		}
	      HEAP_CACHE_ATTRINFO attrs;
	      ASSERT_EQ (heap_attrinfo_start (thread, &h.class_oid, (int) ids.size (), ids.data (), &attrs), NO_ERROR);

	      // A good read first, so the slots hold owned values when the failing read starts.
	      ASSERT_EQ (read_then_drop_record (r.oid, r.bytes, attrs), NO_ERROR) << label;

	      std::vector<char> bad = r.bytes;
	      int location = -1;
	      for (int i = 0; i < attrs.num_values; ++i)
		{
		  if (attrs.values[i].attrid == ids[p.damaged])
		    {
		      location = attrs.values[i].read_attrepr->location;
		    }
		}
	      ASSERT_TRUE (damage_stub (bad, location, how)) << label;
	      const int err = read_then_drop_record (r.oid, bad, attrs);
	      EXPECT_NE (err, NO_ERROR) << label;
	      printf ("[lifetime-err] %s -> %d (er_errid %d)\n", label.c_str (), err, er_errid ());
	      er_clear ();

	      // Every slot is either the previous owned value, this row's owned value, or NULL. Reading it
	      // must not touch released memory.
	      for (int c = 0; c < p.requested; ++c)
		{
		  std::string got = fp (heap_attrinfo_access (ids[c], &attrs));
		  EXPECT_TRUE (got == many_expected (r.id, c) || got == "NULL")
		      << label << " slot c" << c << " after failure holds " << got.substr (0, 40);
		}

	      ASSERT_EQ (read_then_drop_record (r.oid, r.bytes, attrs), NO_ERROR) << label;
	      for (int c = 0; c < p.requested; ++c)
		{
		  EXPECT_EQ (fp (heap_attrinfo_access (ids[c], &attrs)), many_expected (r.id, c)) << label << " c" << c;
		}
	      heap_attrinfo_end (thread, &attrs);
	    }
	}
    }
}

// ==================================================================================================
// L5: pending records (deferred write). Read grouped, paired and scalar while the values live only in
// the pending owner, destroy the owner, then use every value. A mixed record with one disk stub and
// pending stubs uses both read_into and oos_read_many in one group.
// ==================================================================================================
TEST_F (OosSqlLifetime, PendingValuesOutlivePendingOwner)
{
  ASSERT_GE (exec_sql ("CREATE TABLE t_lt_pend (id INT, a BIT VARYING STORAGE FORCE_OUTLINE, "
		       "b VARCHAR STORAGE FORCE_OUTLINE, c BIT VARYING STORAGE FORCE_OUTLINE)"), 0);
  ASSERT_GE (exec_sql (("INSERT INTO t_lt_pend VALUES (99, X'" + hex_of (std::string (7000, '\x77')) + "', '"
			+ std::string (6000, 'D') + "', X'" + hex_of (std::string (300, '\x33')) + "')").c_str ()), 0);
  ASSERT_EQ (db_commit_transaction (), NO_ERROR);
  class_handle h;
  ASSERT_TRUE (open_class ("t_lt_pend", h));
  std::vector<lt_row> disk_rows;
  ASSERT_TRUE (collect_rows (h, disk_rows));
  ASSERT_EQ (disk_rows.size (), 1u);
  THREAD_ENTRY *thread = thread_get_thread_entry_info ();
  const ATTR_ID id_a = attr_id (h, "a"), id_b = attr_id (h, "b"), id_c = attr_id (h, "c"), id_id = attr_id (h, "id");

  const std::string a_bytes (5000, '\xAB');	// scalar: fits the stack scratch
  const std::string b_text (12000, 'P');
  const std::string c_bytes (50000, '\xCD');	// scalar: heap buffer
  const std::string exp_a = "B" + std::to_string (a_bytes.size () * 8) + ":" + a_bytes;
  const std::string exp_b = "S:" + b_text;
  const std::string exp_c = "B" + std::to_string (c_bytes.size () * 8) + ":" + c_bytes;
  const std::string disk_b = "S:" + std::string (6000, 'D');

  for (int mixed = 0; mixed < 2; ++mixed)
    {
      auto storage = std::make_unique<heap_pending_record> ();
      {
	HEAP_CACHE_ATTRINFO attrs;
	ASSERT_EQ (heap_attrinfo_start (thread, &h.class_oid, -1, nullptr, &attrs), NO_ERROR);
	DB_VALUE value;
	db_make_int (&value, 7);
	ASSERT_EQ (heap_attrinfo_set (nullptr, id_id, &value, &attrs), NO_ERROR);
	db_make_varbit (&value, DB_MAX_VARBIT_PRECISION, a_bytes.data (), a_bytes.size () * 8);
	ASSERT_EQ (heap_attrinfo_set (nullptr, id_a, &value, &attrs), NO_ERROR);
	db_make_varchar (&value, DB_MAX_VARCHAR_PRECISION, b_text.data (), b_text.size (), LANG_SYS_CODESET,
			 LANG_SYS_COLLATION);
	ASSERT_EQ (heap_attrinfo_set (nullptr, id_b, &value, &attrs), NO_ERROR);
	db_make_varbit (&value, DB_MAX_VARBIT_PRECISION, c_bytes.data (), c_bytes.size () * 8);
	ASSERT_EQ (heap_attrinfo_set (nullptr, id_c, &value, &attrs), NO_ERROR);
	ASSERT_EQ (heap_attrinfo_prepare_record (thread, &attrs, nullptr, storage.get ()), S_SUCCESS);
	heap_attrinfo_end (thread, &attrs);
      }
      RECDES record = storage->get_recdes ();
      ASSERT_TRUE (heap_recdes_contains_oos (&record));

      HEAP_CACHE_ATTRINFO probe;
      ASSERT_EQ (heap_attrinfo_start (thread, &h.class_oid, -1, nullptr, &probe), NO_ERROR);
      const int loc_b = location_of (probe, id_b);
      heap_attrinfo_end (thread, &probe);
      std::string saved_stub;
      if (mixed)
	{
	  // Replace b's pending stub with the committed row's disk stub of the same type.
	  RECDES disk = { (int) disk_rows[0].bytes.size (), (int) disk_rows[0].bytes.size (), REC_HOME,
			  disk_rows[0].bytes.data ()
			};
	  char *disk_stub = nullptr, *pending_stub = nullptr;
	  ASSERT_EQ (heap_recdes_get_oos_inline_stub (&disk, loc_b, &disk_stub), NO_ERROR);
	  ASSERT_EQ (heap_recdes_get_oos_inline_stub (&record, loc_b, &pending_stub), NO_ERROR);
	  saved_stub.assign (pending_stub, OR_OOS_INLINE_SIZE);
	  std::memcpy (pending_stub, disk_stub, OR_OOS_INLINE_SIZE);
	}
      const std::string want_b = mixed ? disk_b : exp_b;

      struct reader
      {
	std::vector<ATTR_ID> ids;
	std::vector<std::string> want;
	unsigned long long calls;
	unsigned long long requests;
	HEAP_CACHE_ATTRINFO attrs;
      };
      std::vector<reader> readers =
      {
	{ { id_id, id_a, id_b, id_c }, { "I:7", exp_a, want_b, exp_c }, mixed ? 1ULL : 0ULL, mixed ? 1ULL : 0ULL, {} },
	{ { id_a, id_c }, { exp_a, exp_c }, 0, 0, {} },
	{ { id_c, id_b }, { exp_c, want_b }, mixed ? 1ULL : 0ULL, mixed ? 1ULL : 0ULL, {} },
	{ { id_a }, { exp_a }, 0, 0, {} },
	{ { id_c }, { exp_c }, 0, 0, {} },
	{ { id_b }, { want_b }, 0, 0, {} },
      };
      for (reader &rd : readers)
	{
	  ASSERT_EQ (heap_attrinfo_start (thread, &h.class_oid, (int) rd.ids.size (), rd.ids.data (), &rd.attrs),
		     NO_ERROR);
	  bridge_oos_debug_counters_reset ();
	  ASSERT_EQ (heap_attrinfo_read_dbvalues (thread, &oid_Null_oid, &record, &rd.attrs, storage.get ()), NO_ERROR);
	  EXPECT_EQ (bridge_oos_debug_counters_get ().read_many_calls, rd.calls) << "mixed " << mixed;
	  EXPECT_EQ (bridge_oos_debug_counters_get ().read_many_requests, rd.requests) << "mixed " << mixed;
	  for (size_t i = 0; i < rd.ids.size (); ++i)
	    {
	      EXPECT_EQ (fp (heap_attrinfo_access (rd.ids[i], &rd.attrs)), rd.want[i]) << "before release, mixed "
		  << mixed << " value " << i;
	    }
	}
      if (mixed)
	{
	  char *pending_stub = nullptr;
	  ASSERT_EQ (heap_recdes_get_oos_inline_stub (&record, loc_b, &pending_stub), NO_ERROR);
	  std::memcpy (pending_stub, saved_stub.data (), OR_OOS_INLINE_SIZE);
	}
      // Release the pending owner, its record and its retained payloads.
      storage.reset ();
      for (reader &rd : readers)
	{
	  for (size_t i = 0; i < rd.ids.size (); ++i)
	    {
	      EXPECT_EQ (fp (heap_attrinfo_access (rd.ids[i], &rd.attrs)), rd.want[i]) << "after release, mixed "
		  << mixed << " value " << i;
	    }
	  heap_attrinfo_end (thread, &rd.attrs);
	}
    }
}

// ==================================================================================================
// L6: multi-column index keys built from OOS values (heap_midxkey_get_value scalar Resolve).
// ==================================================================================================
TEST_F (OosSqlLifetime, MultiColumnIndexKeysFromOosValues)
{
  ASSERT_GE (exec_sql ("CREATE TABLE t_lt_idx (id INT, a VARCHAR(600) STORAGE FORCE_OUTLINE, "
		       "b VARCHAR(600) STORAGE FORCE_OUTLINE, big BIT VARYING STORAGE FORCE_OUTLINE)"), 0);
  ASSERT_GE (exec_sql ("CREATE UNIQUE INDEX u_lt_ab ON t_lt_idx (a, b)"), 0);
  ASSERT_GE (exec_sql ("CREATE INDEX i_lt_ba ON t_lt_idx (b, id, a)"), 0);
  ASSERT_EQ (db_commit_transaction (), NO_ERROR);
  auto a_of = [] (int id)
  {
    return "a" + std::to_string (id) + std::string (300, 'x');
  };
  auto b_of = [] (int id)
  {
    return "b" + std::to_string (id % 7) + std::string (350, 'y');
  };
  for (int id = 0; id < 40; ++id)
    {
      std::string sql = "INSERT INTO t_lt_idx VALUES (" + std::to_string (id) + ", '" + a_of (id) + "', '" + b_of (id)
			+ "', X'" + hex_of (std::string (id % 2 ? 20000 : 500, (char) id)) + "')";
      ASSERT_GE (exec_sql (sql.c_str ()), 0);
    }
  ASSERT_EQ (db_commit_transaction (), NO_ERROR);
  // A duplicate (a, b) must be rejected by key comparison.
  EXPECT_LT (exec_sql (("INSERT INTO t_lt_idx VALUES (100, '" + a_of (3) + "', '" + b_of (3) + "', NULL)").c_str ()), 0);
  db_abort_transaction ();

  for (int round = 0; round < 2; ++round)
    {
      for (int id = 0; id < 40; ++id)
	{
	  int found = -1;
	  std::string sql = "SELECT COUNT(*) FROM t_lt_idx USE INDEX (u_lt_ab) WHERE a = '" + a_of (id) + "' AND b = '"
			    + b_of (id) + "'";
	  ASSERT_EQ (fetch_single_int (sql.c_str (), &found), NO_ERROR);
	  EXPECT_EQ (found, 1) << "round " << round << " id " << id;
	}
      int by_b = -1;
      ASSERT_EQ (fetch_single_int (("SELECT COUNT(*) FROM t_lt_idx USE INDEX (i_lt_ba) WHERE b = '" + b_of (3)
				    + "' AND id >= 0").c_str (), &by_b), NO_ERROR);
      EXPECT_EQ (by_b, round == 0 ? 6 : 6);
      // Key maintenance on UPDATE reads old and new keys from OOS-backed records.
      ASSERT_GE (exec_sql ("UPDATE t_lt_idx SET id = id + 0, big = big WHERE id >= 0"), 0);
      ASSERT_EQ (db_commit_transaction (), NO_ERROR);
    }
  ASSERT_GE (exec_sql ("DELETE FROM t_lt_idx WHERE id % 3 = 0"), 0);
  ASSERT_EQ (db_commit_transaction (), NO_ERROR);
  int left = -1;
  ASSERT_EQ (fetch_single_int ("SELECT COUNT(*) FROM t_lt_idx USE INDEX (u_lt_ab) WHERE a > '' AND b > ''", &left),
	     NO_ERROR);
  EXPECT_EQ (left, 26);
}

// ==================================================================================================
// L7: scalar Resolve around the stack scratch size (IO_MAX_PAGE_SIZE): OOS lengths just below, equal to
// and just above it, read alone (scalar) and paired (grouped), then checked after the record is freed.
// ==================================================================================================
TEST_F (OosSqlLifetime, ScalarScratchBoundary)
{
  ASSERT_GE (exec_sql ("CREATE TABLE t_lt_scr (id INT, b BIT VARYING STORAGE FORCE_OUTLINE, "
		       "v VARCHAR STORAGE FORCE_OUTLINE)"), 0);
  for (int n = IO_MAX_PAGE_SIZE - 16; n <= IO_MAX_PAGE_SIZE + 8; ++n)
    {
      std::string sql = "INSERT INTO t_lt_scr VALUES (" + std::to_string (n) + ", X'"
			+ hex_of (std::string (n, (char) (n & 0x7F))) + "', '" + std::string (n, (char) ('A' + n % 26))
			+ "')";
      ASSERT_GE (exec_sql (sql.c_str ()), 0);
    }
  ASSERT_EQ (db_commit_transaction (), NO_ERROR);
  class_handle h;
  ASSERT_TRUE (open_class ("t_lt_scr", h));
  std::vector<lt_row> rows;
  ASSERT_TRUE (collect_rows (h, rows));
  THREAD_ENTRY *thread = thread_get_thread_entry_info ();
  const ATTR_ID id_b = attr_id (h, "b"), id_v = attr_id (h, "v");

  HEAP_CACHE_ATTRINFO probe;
  ASSERT_EQ (heap_attrinfo_start (thread, &h.class_oid, -1, nullptr, &probe), NO_ERROR);
  const int loc_b = location_of (probe, id_b), loc_v = location_of (probe, id_v);
  heap_attrinfo_end (thread, &probe);

  int below = 0, equal = 0, above = 0;
  for (const lt_row &r : rows)
    {
      const int n = r.id;
      RECDES rec = { (int) r.bytes.size (), (int) r.bytes.size (), REC_HOME, (char *) r.bytes.data () };
      std::size_t len_b = 0, len_v = 0;
      for (int which = 0; which < 2; ++which)
	{
	  heap_oos_value_ref ref;
	  ASSERT_EQ (heap_oos_value_ref::decode (rec, which ? loc_v : loc_b, ref), NO_ERROR);
	  (which ? len_v : len_b) = ref.length ();
	}
      for (std::size_t len :
	   {
		   len_b, len_v
	   })
	{
	  below += len < (std::size_t) IO_MAX_PAGE_SIZE;
	  equal += len == (std::size_t) IO_MAX_PAGE_SIZE;
	  above += len > (std::size_t) IO_MAX_PAGE_SIZE;
	}
      const std::string want_b = "B" + std::to_string (n * 8) + ":" + std::string (n, (char) (n & 0x7F));
      const std::string want_v = "S:" + std::string (n, (char) ('A' + n % 26));
      const std::vector<std::vector<ATTR_ID>> requests = { { id_b }, { id_v }, { id_b, id_v } };
      for (const auto &ids : requests)
	{
	  HEAP_CACHE_ATTRINFO attrs;
	  ASSERT_EQ (heap_attrinfo_start (thread, &h.class_oid, (int) ids.size (), ids.data (), &attrs), NO_ERROR);
	  for (int pass = 0; pass < 2; ++pass)
	    {
	      ASSERT_EQ (read_then_drop_record (r.oid, r.bytes, attrs), NO_ERROR);
	      for (ATTR_ID id : ids)
		{
		  EXPECT_EQ (fp (heap_attrinfo_access (id, &attrs)), id == id_b ? want_b : want_v)
		      << "n=" << n << " len_b=" << len_b << " len_v=" << len_v << " request size " << ids.size ();
		}
	    }
	  heap_attrinfo_end (thread, &attrs);
	}
      // The client path resolves the same values through its own transform.
      auto out = query_rows_after_end ("SELECT b, v FROM t_lt_scr WHERE id = " + std::to_string (n), 2);
      ASSERT_EQ (out.size (), 1u);
      EXPECT_EQ (out[0][0], want_b) << "n=" << n;
      EXPECT_EQ (out[0][1], want_v) << "n=" << n;
    }
  printf ("[lifetime] OOS lengths around IO_MAX_PAGE_SIZE: below=%d equal=%d above=%d\n", below, equal, above);
  EXPECT_GT (below, 0);
  EXPECT_GT (equal, 0);
  EXPECT_GT (above, 0);
}

// Last test: report every unreachable allocation while the database is still up (db_shutdown hits an
// unrelated ASan report in db_ws_free that aborts before the exit-time leak check would run).
TEST_F (OosSqlLifetime, ZzRecoverableLeakCheck)
{
#if defined(__SANITIZE_ADDRESS__)
  const int leaked = __lsan_do_recoverable_leak_check ();
  printf ("[lifetime] recoverable leak check returned %d (inspect the LSan log for OOS read-path frames)\n", leaked);
#else
  GTEST_SKIP () << "needs an AddressSanitizer build";
#endif
}

int
main (int argc, char **argv)
{
  ::testing::InitGoogleTest (&argc, argv);
  ::testing::AddGlobalTestEnvironment (new SqlServerEnv ());
  return RUN_ALL_TESTS ();
}
