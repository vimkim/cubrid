/*
 * Adversary a1 repro tests for CUBRID PR #8299 (CBRD-27785): variable-offset decoding and bounds.
 *
 * Every case copies a real heap record, applies one in-memory corruption, places the bytes against a
 * PROT_NONE guard page (so any read past recdes->length faults) and drives the engine entry points
 * under test inside a forked child. A child crash is reported as a result line and the sweep resumes
 * with the next case, so the same case list can be run against the head and base engines and diffed.
 *
 * Output: one TSV per sweep under $A1_OUT (default ./a1_out): "<case name>\t<result>".
 */

#include "test_oos_sql_common.hpp"

#include "a1_flavor.hpp"
#include "heap_attrinfo.h"
#include "log_impl.h"
#include "object_domain.h"
#include "object_representation.h"
#include "scope_exit.hpp"
#include "system_parameter.h"
#include "work_space.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// XXX: SHOULD BE THE LAST INCLUDE HEADER
#include "memory_wrapper.hpp"

/* bridge functions defined in oos_file.cpp (CUBRID_UNIT_TEST_ENABLED builds) */
void bridge_oos_debug_counters_reset ();
oos_debug_counters bridge_oos_debug_counters_get ();

namespace
{
  // ---------------------------------------------------------------------------------------------
  // isolated task runner
  // ---------------------------------------------------------------------------------------------
  struct task
  {
    std::string name;
    std::function<std::string ()> fn;
  };

  std::string
  sanitize (const std::string &s)
  {
    std::string out;
    for (char c : s)
      {
	out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
      }
    return out;
  }

  std::vector<std::string>
  run_tasks_all (const std::vector<task> &tasks)
  {
    std::vector<std::string> results (tasks.size (), "<not run>");
    size_t next = 0;
    int respawns = 0;
    while (next < tasks.size ())
      {
	int fds[2];
	if (pipe (fds) != 0)
	  {
	    abort ();
	  }
	fflush (nullptr);
	pid_t pid = fork ();
	if (pid < 0)
	  {
	    abort ();
	  }
	if (pid == 0)
	  {
	    close (fds[0]);
	    int dn = open ("/dev/null", O_WRONLY);
	    dup2 (dn, 1);
	    if (getenv ("A1_STDERR") != nullptr)
	      {
		int ef = open (getenv ("A1_STDERR"), O_WRONLY | O_CREAT | O_APPEND, 0644);
		dup2 (ef, 2);
	      }
	    else
	      {
		dup2 (dn, 2);
	      }
	    for (size_t i = next; i < tasks.size (); ++i)
	      {
		dprintf (fds[1], "S %zu\n", i);
		if (getenv ("A1_STDERR") != nullptr)
		  {
		    fprintf (stderr, "TASK %zu %s\n", i, tasks[i].name.c_str ());
		    fflush (stderr);
		  }
		alarm (20);
		std::string r = sanitize (tasks[i].fn ());
		alarm (0);
		dprintf (fds[1], "R %zu %s\n", i, r.c_str ());
	      }
	    _exit (0);
	  }
	close (fds[1]);
	std::string buf;
	char chunk[65536];
	ssize_t n;
	while ((n = read (fds[0], chunk, sizeof (chunk))) > 0)
	  {
	    buf.append (chunk, (size_t) n);
	  }
	close (fds[0]);
	int status = 0;
	waitpid (pid, &status, 0);

	size_t last_started = next;
	bool last_done = true;
	std::istringstream in (buf);
	std::string line;
	while (std::getline (in, line))
	  {
	    if (line.size () > 2 && line[0] == 'S')
	      {
		last_started = (size_t) std::stoull (line.substr (2));
		last_done = false;
	      }
	    else if (line.size () > 2 && line[0] == 'R')
	      {
		size_t sp = line.find (' ', 2);
		size_t idx = (size_t) std::stoull (line.substr (2, sp - 2));
		results[idx] = sp == std::string::npos ? "" : line.substr (sp + 1);
		last_done = true;
	      }
	  }
	if (WIFSIGNALED (status))
	  {
	    results[last_started] = "CRASH sig=" + std::to_string (WTERMSIG (status));
	    next = last_started + 1;
	    ++respawns;
	  }
	else if (!last_done)
	  {
	    results[last_started] = "EXIT code=" + std::to_string (WEXITSTATUS (status));
	    next = last_started + 1;
	    ++respawns;
	  }
	else
	  {
	    next = tasks.size ();
	  }
      }
    printf ("a1: %zu tasks, %d child respawns\n", tasks.size (), respawns);
    return results;
  }


  std::vector<std::string>
  run_tasks (const std::vector<task> &tasks)
  {
    const char *only = getenv ("A1_ONLY");
    if (only == nullptr)
      {
	return run_tasks_all (tasks);
      }
    std::vector<task> sel;
    std::vector<size_t> where;
    for (size_t i = 0; i < tasks.size (); ++i)
      {
	if (tasks[i].name.find (only) != std::string::npos)
	  {
	    sel.push_back (tasks[i]);
	    where.push_back (i);
	  }
      }
    std::vector<std::string> sub = run_tasks_all (sel);
    std::vector<std::string> results (tasks.size (), "<skipped>");
    for (size_t i = 0; i < where.size (); ++i)
      {
	results[where[i]] = sub[i];
      }
    return results;
  }

  void
  write_tsv (const char *sweep, const std::vector<task> &tasks, const std::vector<std::string> &results)
  {
    const char *dir = getenv ("A1_OUT");
    std::string d = dir ? dir : "./a1_out";
    mkdir (d.c_str (), 0755);
    std::string path = d + "/" + sweep + ".tsv";
    FILE *f = fopen (path.c_str (), "w");
    ASSERT_NE (f, nullptr) << path;
    for (size_t i = 0; i < tasks.size (); ++i)
      {
	fprintf (f, "%s\t%s\n", tasks[i].name.c_str (), results[i].c_str ());
      }
    fclose (f);
  }

  // ---------------------------------------------------------------------------------------------
  // guard page placement
  // ---------------------------------------------------------------------------------------------
  struct guard_region
  {
    char *base = nullptr;
    size_t page = 0;
    char *data_page () const
    {
      return base + page;
    }
  };

  guard_region &
  region ()
  {
    static guard_region r;
    if (r.base == nullptr)
      {
	r.page = (size_t) sysconf (_SC_PAGESIZE);
	void *m = mmap (nullptr, 3 * r.page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED)
	  {
	    abort ();
	  }
	r.base = (char *) m;
	mprotect (r.base, r.page, PROT_NONE);
	mprotect (r.base + 2 * r.page, r.page, PROT_NONE);
      }
    return r;
  }

  // placement 0: record ends exactly at the trailing guard page. 1: record starts exactly after the leading guard page.
  char *
  place (const std::vector<char> &bytes, int length, int placement)
  {
    guard_region &r = region ();
    memset (r.data_page (), 0xCD, r.page);
    char *dest = placement == 0 ? r.data_page () + r.page - length : r.data_page ();
    memcpy (dest, bytes.data (), (size_t) length);
    return dest;
  }

  // ---------------------------------------------------------------------------------------------
  // value description
  // ---------------------------------------------------------------------------------------------
  std::string
  hash_bytes (const char *p, size_t n)
  {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i)
      {
	h ^= (unsigned char) p[i];
	h *= 1099511628211ULL;
      }
    char b[40];
    snprintf (b, sizeof (b), "%016llx", (unsigned long long) h);
    return b;
  }

  std::string
  describe (DB_VALUE *v)
  {
    if (v == nullptr)
      {
	return "<nullptr>";
      }
    std::ostringstream o;
    DB_TYPE t = db_value_type (v);
    o << "t" << (int) t;
    if (DB_IS_NULL (v))
      {
	o << ":NULL";
	return o.str ();
      }
    switch (t)
      {
      case DB_TYPE_INTEGER:
	o << ":" << db_get_int (v);
	break;
      case DB_TYPE_DOUBLE:
	o << ":" << db_get_double (v);
	break;
      case DB_TYPE_NUMERIC:
	o << ":h=" << hash_bytes ((const char *) db_get_numeric (v), 16);
	break;
      case DB_TYPE_CHAR:
      case DB_TYPE_VARCHAR:
      {
	const char *s = db_get_string (v);
	int sz = db_get_string_size (v);
	o << ":sz=" << sz << " h=" << (s && sz >= 0 ? hash_bytes (s, (size_t) sz) : std::string ("<null>"));
      }
      break;
      case DB_TYPE_BIT:
      case DB_TYPE_VARBIT:
      {
	int bits = 0;
	const char *s = db_get_bit (v, &bits);
	o << ":bits=" << bits << " h=" << (s && bits >= 0 ? hash_bytes (s, (size_t) (bits + 7) / 8) : std::string ("<null>"));
      }
      break;
      case DB_TYPE_SEQUENCE:
      case DB_TYPE_SET:
      case DB_TYPE_MULTISET:
      {
	DB_SET *set = db_get_set (v);
	int sz = set ? db_set_size (set) : -1;
	o << ":n=" << sz << "[";
	for (int i = 0; i < sz; ++i)
	  {
	    DB_VALUE e;
	    db_make_null (&e);
	    if (db_set_get (set, i, &e) == NO_ERROR)
	      {
		o << (DB_IS_NULL (&e) ? std::string ("N") : std::to_string (db_get_int (&e))) << ",";
	      }
	    db_value_clear (&e);
	  }
	o << "]";
      }
      break;
      default:
	o << ":?";
      }
    o << " p=" << db_value_precision (v) << " s=" << db_value_scale (v);
    return o.str ();
  }

  // ---------------------------------------------------------------------------------------------
  // record context
  // ---------------------------------------------------------------------------------------------
  struct row_ctx
  {
    int label;
    OID oid;
    std::vector<char> bytes;
    int header;
    int width;
    int nvar;
    int reprid;
    std::string oos_locations;
  };

  struct subset_ctx
  {
    std::string name;
    std::vector<ATTR_ID> ids;
  };

  const char *ALL_NAMES[] = { "id", "v1", "big1", "v2", "d1", "s1", "big2", "v3", "n1", "c1", "e1", "added" };

  int
  get_entry (const char *vt, int idx, int w)
  {
    const char *p = vt + idx * w;
    return w == 1 ? (int) OR_GET_BYTE (p) : (w == 2 ? (int) (unsigned short) OR_GET_SHORT (p) : (int) OR_GET_INT (p));
  }

  void
  put_entry (char *vt, int idx, int w, unsigned int val)
  {
    OR_PUT_OFFSET_INTERNAL (vt + idx * w, val, w);
  }

  bool
  entry_fits (unsigned int val, int w)
  {
    return w == 1 ? val <= 255 : (w == 2 ? val <= 32767 : true);
  }

  // Re-encode a valid record with another variable-offset width. Returns false when an offset does not fit.
  bool
  repack (const row_ctx &row, int new_w, std::vector<char> &out)
  {
    const int old_w = row.width;
    const int old_vot = OR_VAR_TABLE_SIZE_INTERNAL (row.nvar, old_w);
    const int new_vot = OR_VAR_TABLE_SIZE_INTERNAL (row.nvar, new_w);
    const int delta = new_vot - old_vot;
    out.assign (row.bytes.begin (), row.bytes.begin () + row.header);
    out.resize (row.header + new_vot, 0);
    out.insert (out.end (), row.bytes.begin () + row.header + old_vot, row.bytes.end ());
    int word = OR_GET_INT (out.data ()) & ~OR_OFFSET_SIZE_FLAG;
    OR_SET_VAR_OFFSET_SIZE (word, new_w);
    OR_PUT_INT (out.data (), word);
    const char *old_t = row.bytes.data () + row.header;
    char *new_t = out.data () + row.header;
    for (int i = 0; i <= row.nvar; ++i)
      {
	int e = get_entry (old_t, i, old_w);
	unsigned int ne = (unsigned int) ((e & ~OR_VAR_FLAG_MASK) + delta) | (unsigned int) (e & OR_VAR_FLAG_MASK);
	if (!entry_fits (ne, new_w))
	  {
	    return false;
	  }
	put_entry (new_t, i, new_w, ne);
      }
    return true;
  }

  // ---------------------------------------------------------------------------------------------
  // one engine read
  // ---------------------------------------------------------------------------------------------
  enum api_kind { API_OID = 0, API_NOOID = 1, API_LAZY = 2 };
  const char *API_NAMES[] = { "read", "readNoOid", "lazy" };

  std::string
  do_read_inner (const row_ctx &row, const subset_ctx &subset, const std::vector<char> &bytes, int length, int placement,
		 api_kind api, OID class_oid)
  {
    THREAD_ENTRY *thread = thread_get_thread_entry_info ();
    char *data = place (bytes, length, placement);
    RECDES rec = { length, length, REC_HOME, data };
    HEAP_CACHE_ATTRINFO attrs;
    int rc = heap_attrinfo_start (thread, &class_oid, (int) subset.ids.size (), subset.ids.data (), &attrs);
    if (rc != NO_ERROR)
      {
	return "start_failed " + std::to_string (rc);
      }
    er_clear ();
    bridge_oos_debug_counters_reset ();
    std::ostringstream o;
    if (api == API_OID)
      {
	rc = heap_attrinfo_read_dbvalues (thread, &row.oid, &rec, &attrs);
      }
    else if (api == API_NOOID)
      {
	rc = heap_attrinfo_read_dbvalues_without_oid (thread, &rec, &attrs);
      }
    else
      {
	rc = heap_attrinfo_read_dbvalues_lazy (thread, &row.oid, &rec, &attrs);
	if (rc == NO_ERROR)
	  {
	    for (int i = 0; i < attrs.num_values; ++i)
	      {
		DB_VALUE *v = heap_attrvalue_peek_lazy (&attrs.values[i], &attrs);
		o << i << "=" << (v == nullptr ? std::string ("PEEKFAIL(er=") + std::to_string (er_errid ()) + ")" :
				  describe (v)) << "; ";
	      }
	    heap_attrinfo_end (thread, &attrs);
	    return "ret=0 " + o.str ();
	  }
      }
    if (rc != NO_ERROR)
      {
	std::string r = "ret=" + std::to_string (rc) + " er=" + std::to_string (er_errid ());
	heap_attrinfo_end (thread, &attrs);
	return r;
      }
    for (size_t i = 0; i < subset.ids.size (); ++i)
      {
	o << i << "=" << describe (heap_attrinfo_access (subset.ids[i], &attrs)) << "; ";
      }
    heap_attrinfo_end (thread, &attrs);
    return "ret=0 " + o.str ();
  }

  std::string
  do_read (const row_ctx &row, const subset_ctx &subset, const std::vector<char> &bytes, int length, int placement,
	   api_kind api, OID class_oid)
  {
    std::string r = do_read_inner (row, subset, bytes, length, placement, api, class_oid);
    const oos_debug_counters c = bridge_oos_debug_counters_get ();
    return r + " |rm=" + std::to_string (c.read_many_calls) + "/" + std::to_string (c.read_many_requests);
  }

  // Valid record with another MVCC header size: only the header changes, offsets are relative to the var table.
  std::vector<char>
  with_mvcc_flags (const std::vector<char> &bytes, int mvcc)
  {
    const int old_hs = OR_HEADER_SIZE ((char *) bytes.data ());
    int hs = 8;
    for (int bit = 0; bit < 3; ++bit)
      {
	if (mvcc & (1 << bit))
	  {
	    hs += 8;
	  }
      }
    std::vector<char> out (hs, (char) 0x5A);
    memcpy (out.data (), bytes.data (), 8);
    unsigned int word = (unsigned int) OR_GET_INT (out.data ());
    word &= ~ ((unsigned int) OR_RECORD_MVCC_FLAG_MASK << OR_RECORD_FLAG_SHIFT_BITS);
    word |= (unsigned int) mvcc << OR_RECORD_FLAG_SHIFT_BITS;
    OR_PUT_INT (out.data (), (int) word);
    out.insert (out.end (), bytes.begin () + old_hs, bytes.end ());
    return out;
  }

  // Read with a caller-owned attr_info (reuse across records). Does not end attrs.
  std::string
  read_reuse (HEAP_CACHE_ATTRINFO &attrs, const subset_ctx &subset, const OID &oid, const std::vector<char> &bytes,
	      api_kind api)
  {
    THREAD_ENTRY *thread = thread_get_thread_entry_info ();
    const int length = (int) bytes.size ();
    char *data = place (bytes, length, 0);
    RECDES rec = { length, length, REC_HOME, data };
    er_clear ();
    bridge_oos_debug_counters_reset ();
    std::ostringstream o;
    int rc;
    if (api == API_OID)
      {
	rc = heap_attrinfo_read_dbvalues (thread, &oid, &rec, &attrs);
      }
    else if (api == API_NOOID)
      {
	rc = heap_attrinfo_read_dbvalues_without_oid (thread, &rec, &attrs);
      }
    else
      {
	rc = heap_attrinfo_read_dbvalues_lazy (thread, &oid, &rec, &attrs);
	if (rc == NO_ERROR)
	  {
	    for (int i = 0; i < attrs.num_values; ++i)
	      {
		DB_VALUE *v = heap_attrvalue_peek_lazy (&attrs.values[i], &attrs);
		o << i << "=" << (v == nullptr ? std::string ("PEEKFAIL") : describe (v)) << "; ";
	      }
	  }
      }
    if (rc != NO_ERROR)
      {
	return "ret=" + std::to_string (rc);
      }
    if (api != API_LAZY)
      {
	for (size_t i = 0; i < subset.ids.size (); ++i)
	  {
	    o << i << "=" << describe (heap_attrinfo_access (subset.ids[i], &attrs)) << "; ";
	  }
      }
    const oos_debug_counters c = bridge_oos_debug_counters_get ();
    return "ret=0 " + o.str () + " |rm=" + std::to_string (c.read_many_calls) + "/" + std::to_string (c.read_many_requests);
  }
}

class OosSqlA1VarOffsets : public ::testing::Test
{
  protected:
    OID class_oid;
    std::vector<row_ctx> rows;
    std::vector<subset_ctx> subsets;
    ATTR_ID name_ids[12];

    void SetUp () override
    {
      // every corrupt-record error would otherwise spawn addr2line for a call-stack dump (seconds each)
      sysprm_set_force (PRM_ID_CALL_STACK_DUMP_ON_ERROR, "no");
      int *activation = prm_get_integer_list_value (PRM_ID_CALL_STACK_DUMP_ACTIVATION);
      if (activation != nullptr)
	{
	  activation[0] = 0;
	}
      exec_sql ("DROP TABLE IF EXISTS t_a1");
      db_commit_transaction ();
      ASSERT_GE (exec_sql ("CREATE TABLE t_a1 (id INT, v1 VARCHAR(100), big1 BIT VARYING, v2 VARCHAR(100), d1 DOUBLE, "
			   "s1 SEQUENCE OF INTEGER, big2 BIT VARYING, v3 VARCHAR(30), n1 NUMERIC(20,5), c1 CHAR(10), "
			   "e1 VARCHAR(20))"), 0);
      ASSERT_GE (exec_sql ("INSERT INTO t_a1 VALUES "
			   "(1, 'alpha', X'0102', 'beta', 1.5, {1,2,3}, X'ff', 'gamma', 12.5, 'abc', ''), "
			   "(2, 'x', CAST(REPEAT('AA', 4500) AS BIT VARYING), 'y', 2.5, {4}, "
			   "CAST(REPEAT('BB', 20000) AS BIT VARYING), 'z', NULL, NULL, 'e'), "
			   "(3, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL), "
			   "(4, '', X'', '', 4.5, {}, X'', '', 0, '', ''), "
			   "(5, 'one', CAST(REPEAT('CC', 4500) AS BIT VARYING), 'two', NULL, {7,8}, X'01', 'three', 1, 'q', 'w'), "
			   "(6, REPEAT('v', 90), CAST(REPEAT('DD', 20000) AS BIT VARYING), REPEAT('w', 90), 1, {1}, "
			   "CAST(REPEAT('EE', 4500) AS BIT VARYING), REPEAT('u', 28), 2, 'c', REPEAT('e', 19))"), 0);
      ASSERT_EQ (db_commit_transaction (), NO_ERROR);
      ASSERT_GE (exec_sql ("ALTER TABLE t_a1 ADD COLUMN added VARCHAR(20) DEFAULT 'dflt'"), 0);
      ASSERT_GE (exec_sql ("INSERT INTO t_a1 VALUES (7, 'new', CAST(REPEAT('FF', 4500) AS BIT VARYING), 'n2', 7.5, {9}, "
			   "X'02', 'n3', 7, 'n', 'n4', 'added7')"), 0);
      ASSERT_EQ (db_commit_transaction (), NO_ERROR);

      THREAD_ENTRY *thread = thread_get_thread_entry_info ();
      DB_OBJECT *cls = db_find_class ("t_a1");
      ASSERT_NE (cls, nullptr);
      class_oid = *WS_OID (cls);
      HFID hfid;
      ASSERT_EQ (heap_get_class_hfid (thread, &class_oid, &hfid, nullptr), NO_ERROR);
      for (int i = 0; i < 12; ++i)
	{
	  DB_ATTRIBUTE *a = db_get_attribute (cls, ALL_NAMES[i]);
	  ASSERT_NE (a, nullptr) << ALL_NAMES[i];
	  name_ids[i] = db_attribute_id (a);
	}

      DB_OBJLIST *objects = db_fetch_all_objects (cls, DB_FETCH_READ);
      ASSERT_NE (objects, nullptr);
      scope_exit free_objects ([&] ()
      {
	db_objlist_free (objects);
      });
      for (DB_OBJLIST *item = objects; item != nullptr; item = item->next)
	{
	  row_ctx row;
	  row.oid = *WS_OID (item->op);
	  HEAP_SCANCACHE scan;
	  ASSERT_EQ (heap_scancache_start (thread, &scan, &hfid, &class_oid, true,
					   logtb_get_mvcc_snapshot (thread)), NO_ERROR);
	  RECDES record = RECDES_INITIALIZER;
	  const SCAN_CODE status = heap_get_visible_version (thread, &row.oid, &class_oid, &record, &scan, COPY, NULL_CHN,
				   HEAP_RECDES_DONT_CONSUME_RAW_BYTES);
	  if (status == S_SUCCESS)
	    {
	      row.bytes.assign (record.data, record.data + record.length);
	    }
	  EXPECT_EQ (heap_scancache_end (thread, &scan), NO_ERROR);
	  ASSERT_EQ (status, S_SUCCESS);
	  row.header = OR_HEADER_SIZE (row.bytes.data ());
	  row.width = OR_GET_OFFSET_SIZE (row.bytes.data ());
	  row.reprid = OR_GET_MVCC_REPID (row.bytes.data ());
	  int cache_index = -1;
	  OR_CLASSREP *repr = heap_classrepr_get (thread, &class_oid, nullptr, row.reprid, &cache_index);
	  ASSERT_NE (repr, nullptr);
	  row.nvar = repr->n_variable;
	  heap_classrepr_free (repr, &cache_index);
	  std::ostringstream o;
	  for (int i = 0; i < row.nvar; ++i)
	    {
	      int e = get_entry (row.bytes.data () + row.header, i, row.width);
	      if (OR_IS_OOS (e))
		{
		  o << i << ",";
		}
	    }
	  row.oos_locations = o.str ();
	  rows.push_back (row);
	}
      {
	// A1_PIN=<rows.hex of an earlier run>: replace the captured bytes with the pinned ones so that head and base
	// are fed byte-identical records (stub OIDs and identity stamps differ between database incarnations)
	const char *pin = getenv ("A1_PIN");
	if (pin != nullptr)
	  {
	    FILE *f = fopen (pin, "r");
	    ASSERT_NE (f, nullptr) << pin;
	    char *line = nullptr;
	    size_t cap = 0;
	    size_t idx = 0;
	    while (getline (&line, &cap, f) > 0 && idx < rows.size ())
	      {
		std::string hex (line);
		while (!hex.empty () && (hex.back () == '\n' || hex.back () == '\r'))
		  {
		    hex.pop_back ();
		  }
		std::vector<char> b;
		for (size_t i = 0; i + 1 < hex.size (); i += 2)
		  {
		    b.push_back ((char) strtol (hex.substr (i, 2).c_str (), nullptr, 16));
		  }
		ASSERT_EQ (b.size (), rows[idx].bytes.size ()) << "pinned row " << idx << " has another length";
		rows[idx].bytes = b;
		++idx;
	      }
	    free (line);
	    fclose (f);
	    ASSERT_EQ (idx, rows.size ());
	  }
      }
      {
	// dump the captured record bytes so head and base runs can be compared byte for byte
	const char *dir = getenv ("A1_OUT");
	std::string d = dir ? dir : "./a1_out";
	mkdir (d.c_str (), 0755);
	FILE *f = fopen ((d + "/rows.hex").c_str (), "w");
	if (f != nullptr)
	  {
	    for (const row_ctx &r : rows)
	      {
		for (char c : r.bytes)
		  {
		    fprintf (f, "%02x", (unsigned char) c);
		  }
		fprintf (f, "\n");
	      }
	    fclose (f);
	  }
      }
      // order rows by the id column value is not needed; label by position in the scan
      for (size_t i = 0; i < rows.size (); ++i)
	{
	  rows[i].label = (int) i;
	}

      subset_ctx all;
      all.name = "ALL";
      for (int i = 0; i < 12; ++i)
	{
	  all.ids.push_back (name_ids[i]);
	}
      subsets.push_back (all);
      subset_ctx vars;
      vars.name = "VARS";
      for (int i :
	   {
		   1, 2, 3, 5, 6, 7, 10, 11
	   })
	{
	  vars.ids.push_back (name_ids[i]);
	}
      subsets.push_back (vars);
      subset_ctx pair;
      pair.name = "OOSPAIR";
      pair.ids = { name_ids[2], name_ids[6] };
      subsets.push_back (pair);
      subset_ctx fixed;
      fixed.name = "FIXED";
      for (int i :
	   {
		   0, 4, 8, 9
	   })
	{
	  fixed.ids.push_back (name_ids[i]);
	}
      subsets.push_back (fixed);
      for (int i :
	   {
		   1, 2, 3, 5, 6, 7, 10, 11
	   })
	{
	  subset_ctx one;
	  one.name = std::string ("only_") + ALL_NAMES[i];
	  one.ids = { name_ids[i] };
	  subsets.push_back (one);
	}
    }

    void TearDown () override
    {
      db_abort_transaction ();
      exec_sql ("DROP TABLE IF EXISTS t_a1");
      db_commit_transaction ();
    }

    std::string tag (const row_ctx &row, const std::string &m, const subset_ctx &s, int api, int placement)
    {
      std::ostringstream o;
      o << "row" << row.label << "(rep" << row.reprid << ",w" << row.width << ",n" << row.nvar << ",hs" << row.header
	<< ",oos[" << row.oos_locations << "],len" << row.bytes.size () << ")/" << m << "/" << s.name << "/"
	<< API_NAMES[api] << "/p" << placement;
      return o.str ();
    }

    void add_read (std::vector<task> &tasks, const row_ctx &row, const std::string &m, const std::vector<char> &bytes,
		   int length, int placement, const subset_ctx &s, int api)
    {
      OID coid = class_oid;
      OID roid = row.oid;
      row_ctx rcopy = row;
      rcopy.oid = roid;
      subset_ctx scopy = s;
      tasks.push_back ({ tag (row, m, s, api, placement),
			 [rcopy, scopy, bytes, length, placement, api, coid] ()
      {
	return do_read (rcopy, scopy, bytes, length, placement, (api_kind) api, coid);
      } });
    }
};

// A1-0: valid records re-encoded at every offset width read identically to the stored record.
TEST_F (OosSqlA1VarOffsets, ValidWidthsEquivalence)
{
  std::vector<task> tasks;
  std::vector<std::pair<size_t, size_t>> pairs; // (orig task, variant task)
  for (const row_ctx &row : rows)
    {
      for (const subset_ctx &s : subsets)
	{
	  for (int api = 0; api < 3; ++api)
	    {
	      const size_t orig_idx = tasks.size ();
	      add_read (tasks, row, "orig", row.bytes, (int) row.bytes.size (), 0, s, api);
	      for (int w :
		   {
			   1, 2, 4
		   })
		{
		  std::vector<char> rp;
		  if (!repack (row, w, rp))
		    {
		      continue;
		    }
		  pairs.push_back ({ orig_idx, tasks.size () });
		  add_read (tasks, row, "w" + std::to_string (w), rp, (int) rp.size (), 0, s, api);
		}
	    }
	}
    }
  std::vector<std::string> results = run_tasks (tasks);
  ASSERT_NO_FATAL_FAILURE (write_tsv ("ValidWidthsEquivalence", tasks, results));
  int mismatches = 0;
  for (auto &pr : pairs)
    {
      if (results[pr.first].compare (0, 5, "ret=0") != 0 || results[pr.first] != results[pr.second])
	{
	  if (mismatches++ < 20)
	    {
	      ADD_FAILURE () << tasks[pr.second].name << " -> " << results[pr.second] << "\n  orig " << tasks[pr.first].name
			     << " -> " << results[pr.first];
	    }
	}
    }
  EXPECT_EQ (mismatches, 0);
}

// A1-1: truncation sweep. Every recdes->length from 0 to the full length, record flush against the guard page.
TEST_F (OosSqlA1VarOffsets, TruncationSweep)
{
  std::vector<task> tasks;
  for (const row_ctx &row : rows)
    {
      for (int w :
	   {
		   0, 1, 2, 4
	   })
	{
	  std::vector<char> bytes = row.bytes;
	  if (w != 0 && !repack (row, w, bytes))
	    {
	      continue;
	    }
	  const std::string m_base = w == 0 ? "stored" : ("repack" + std::to_string (w));
	  const int total = (int) bytes.size ();
	  const int ow = OR_GET_OFFSET_SIZE (bytes.data ());
	  const int window = row.header + OR_VAR_TABLE_SIZE_INTERNAL (row.nvar, ow) + 8;
	  std::vector<int> lens;
	  for (int len = 0; len <= std::min (total, window); ++len)
	    {
	      lens.push_back (len);
	    }
	  for (int len :
	       {
		       total / 2, total - 8, total - 4, total - 3, total - 2, total - 1, total
	       })
	    {
	      if (len > window && len <= total)
		{
		  lens.push_back (len);
		}
	    }
	  for (int len : lens)
	    {
	      for (const subset_ctx &s : subsets)
		{
		  if (s.name != "ALL" && s.name != "VARS" && s.name != "OOSPAIR" && s.name != "only_e1" && s.name != "only_big1"
		      && s.name != "only_v1" && s.name != "only_added" && s.name != "only_s1")
		    {
		      continue;
		    }
		  for (int api :
		       {
			       0, 2
		       })
		    {
		      add_read (tasks, row, m_base + "/trunc" + std::to_string (len), bytes, len, 0, s, api);
		    }
		}
	    }
	}
    }
  std::vector<std::string> results = run_tasks (tasks);
  ASSERT_NO_FATAL_FAILURE (write_tsv ("TruncationSweep", tasks, results));
}

// A1-2: header word mutations (MVCC flag bits that change OR_HEADER_SIZE, offset-size field, HAS_OOS, bound bit).
TEST_F (OosSqlA1VarOffsets, HeaderFlagSweep)
{
  std::vector<task> tasks;
  for (const row_ctx &row : rows)
    {
      for (int w :
	   {
		   1, 2, 4
	   })
	{
	  std::vector<char> rp;
	  if (!repack (row, w, rp))
	    {
	      continue;
	    }
	  for (int mvcc = 0; mvcc < 8; ++mvcc)
	    {
	      for (int has_oos :
		   {
			   0, 1
		   })
		{
		  for (int bound :
		       {
			       0, 1
		       })
		    {
		      for (int ow :
			   {
				   1, 2, 4
			   })
			{
			  std::vector<char> bytes = rp;
			  unsigned int word = (unsigned int) OR_GET_INT (bytes.data ());
			  word &= ~ (unsigned int) OR_OFFSET_SIZE_FLAG;
			  word &= ~ (unsigned int) OR_BOUND_BIT_FLAG;
			  word &= ~ ((unsigned int) OR_RECORD_FLAG_MASK << OR_RECORD_FLAG_SHIFT_BITS);
			  word |= ((unsigned int) mvcc | (has_oos ? OR_RECORD_FLAG_HAS_OOS : 0)) << OR_RECORD_FLAG_SHIFT_BITS;
			  if (bound)
			    {
			      word |= (unsigned int) OR_BOUND_BIT_FLAG;
			    }
			  int iw = (int) word;
			  OR_SET_VAR_OFFSET_SIZE (iw, ow);
			  OR_PUT_INT (bytes.data (), iw);
			  std::string m = "w" + std::to_string (w) + "to" + std::to_string (ow) + "/mvcc" + std::to_string (mvcc)
					  + "/oosflag" + std::to_string (has_oos) + "/bound" + std::to_string (bound);
			  for (const subset_ctx &s : subsets)
			    {
			      if (s.name != "ALL" && s.name != "VARS" && s.name != "OOSPAIR" && s.name != "only_e1"
				  && s.name != "only_big1" && s.name != "only_v1")
				{
				  continue;
				}
			      for (int api :
				   {
					   0, 2
				   })
				{
				  add_read (tasks, row, m, bytes, (int) bytes.size (), 0, s, api);
				}
			    }
			}
		    }
		}
	    }
	}
    }
  std::vector<std::string> results = run_tasks (tasks);
  ASSERT_NO_FATAL_FAILURE (write_tsv ("HeaderFlagSweep", tasks, results));
}

// A1-3: single VOT entry corruption (offset past the record end, negative, OOS flag on short field, ...).
TEST_F (OosSqlA1VarOffsets, VotEntrySweep)
{
  std::vector<task> tasks;
  for (const row_ctx &row : rows)
    {
      for (int w :
	   {
		   1, 2, 4
	   })
	{
	  std::vector<char> rp;
	  if (!repack (row, w, rp))
	    {
	      continue;
	    }
	  const int len = (int) rp.size ();
	  const int body = len - row.header;
	  struct pat
	  {
	    const char *name;
	    std::function<bool (int entry, int next, unsigned int &out)> f;
	  };
	  const unsigned int maxv = w == 1 ? 0xf0 : (w == 2 ? 0x7ff0 : 0x7ffffff0u);
	  std::vector<pat> pats =
	  {
	    {
	      "endpast", [&] (int e, int, unsigned int &o)
	      {
		o = (unsigned int) (body + 0) | (e & 3);
		return entry_fits (o, w);
	      }
	    },
	    {
	      "endpast4", [&] (int e, int, unsigned int &o)
	      {
		o = (unsigned int) (body + 4) | (e & 3);
		return entry_fits (o, w);
	      }
	    },
	    {
	      "endpast64", [&] (int e, int, unsigned int &o)
	      {
		o = (unsigned int) (body + 64) | (e & 3);
		return entry_fits (o, w);
	      }
	    },
	    {
	      "huge", [&] (int e, int, unsigned int &o)
	      {
		o = maxv | (e & 3);
		return true;
	      }
	    },
	    {
	      "neg", [&] (int, int, unsigned int &o)
	      {
		o = w == 1 ? 0xfcu : (w == 2 ? 0xfff0u : 0xfffffff0u);
		return true;
	      }
	    },
	    {
	      "zero", [&] (int, int, unsigned int &o)
	      {
		o = 0;
		return true;
	      }
	    },
	    {
	      "oosflag", [&] (int e, int, unsigned int &o)
	      {
		o = (unsigned int) e | OR_VAR_BIT_OOS;
		return true;
	      }
	    },
	    {
	      "oos+last", [&] (int e, int, unsigned int &o)
	      {
		o = (unsigned int) e | OR_VAR_BIT_OOS | OR_VAR_BIT_LAST_ELEMENT;
		return true;
	      }
	    },
	    {
	      "last", [&] (int e, int, unsigned int &o)
	      {
		o = (unsigned int) e | OR_VAR_BIT_LAST_ELEMENT;
		return true;
	      }
	    },
	    {
	      "eqnext", [&] (int, int n, unsigned int &o)
	      {
		o = (unsigned int) n;
		return true;
	      }
	    },
	    {
	      "clearoos", [&] (int e, int, unsigned int &o)
	      {
		o = (unsigned int) e & ~OR_VAR_BIT_OOS;
		return true;
	      }
	    },
	  };
	  for (int k = 0; k <= row.nvar; ++k)
	    {
	      for (const pat &p : pats)
		{
		  std::vector<char> bytes = rp;
		  char *vt = bytes.data () + row.header;
		  int e = get_entry (vt, k, w);
		  int n = k < row.nvar ? get_entry (vt, k + 1, w) : e;
		  unsigned int out;
		  if (!p.f (e, n, out))
		    {
		      continue;
		    }
		  put_entry (vt, k, w, out);
		  std::string m = "w" + std::to_string (w) + "/vot" + std::to_string (k) + "=" + p.name;
		  for (const subset_ctx &s : subsets)
		    {
		      for (int api :
			   {
				   0, 2
			   })
			{
			  add_read (tasks, row, m, bytes, len, 0, s, api);
			}
		    }
		}
	    }
	}
    }
  std::vector<std::string> results = run_tasks (tasks);
  ASSERT_NO_FATAL_FAILURE (write_tsv ("VotEntrySweep", tasks, results));
}

// A1-4: the three public accessors, location x length x width, compared with a closed-form oracle.
TEST_F (OosSqlA1VarOffsets, DirectAccessorOracle)
{
  std::vector<task> tasks;
  for (const row_ctx &row : rows)
    {
      for (int w :
	   {
		   0, 1, 2, 4
	   })
	{
	  std::vector<char> bytes = row.bytes;
	  if (w != 0 && !repack (row, w, bytes))
	    {
	      continue;
	    }
	  for (int len = 0; len <= (int) bytes.size (); ++len)
	    {
	      const std::string name = "row" + std::to_string (row.label) + "/w" + std::to_string (w) + "/len"
				       + std::to_string (len) + "/n" + std::to_string (row.nvar);
	      const int nvar = row.nvar;
	      tasks.push_back ({ name, [bytes, len, nvar] ()
	      {
		char *data = place (bytes, len, 0);
		RECDES rec = { len, len, REC_HOME, data };
		std::ostringstream o;
		int mismatches = 0;
		const bool valid_header = len >= OR_MVCC_MIN_HEADER_SIZE && OR_HEADER_SIZE (bytes.data ()) <= len;
		const int hs = valid_header ? OR_HEADER_SIZE (bytes.data ()) : 0;
		const int ow = OR_GET_OFFSET_SIZE (bytes.data ());
		for (int loc = -2; loc <= nvar + 2; ++loc)
		  {
		    int e1 = -77, e2 = -77, n2 = -77;
		    int r_entry = heap_recdes_get_var_offset_entry (&rec, loc, &e1);
#if A1_HAS_PAIR
		    int r_pair = heap_recdes_get_var_offset_pair (&rec, loc, &e2, &n2);
#else
		    int r_pair = heap_recdes_get_var_offset_entry (&rec, loc, &e2);
		    if (r_pair == NO_ERROR)
		      {
			r_pair = heap_recdes_get_var_offset_entry (&rec, loc + 1, &n2);
		      }
#endif
		    char *stub = nullptr;
		    int r_stub = heap_recdes_get_oos_inline_stub (&rec, loc, &stub);
		    const bool exp_entry = valid_header && loc >= 0 && hs + (long) (loc + 1) * ow <= len;
		    const bool exp_pair = valid_header && loc >= 0 && hs + (long) (loc + 2) * ow <= len;
		    bool exp_stub = false;
		    if (exp_pair)
		      {
			int te = get_entry (bytes.data () + hs, loc, ow);
			int ne = get_entry (bytes.data () + hs, loc + 1, ow);
			if (ow == 2)
			  {
			    te = (short) te;
			    ne = (short) ne;
			  }
			int to = OR_GET_VAR_OFFSET (te), no = OR_GET_VAR_OFFSET (ne);
			exp_stub = OR_IS_OOS (te) && to >= (loc + 2) * ow && no - to == OR_OOS_INLINE_SIZE && hs + no <= len;
		      }
		    bool bad = ((r_entry == NO_ERROR) != exp_entry) || ((r_pair == NO_ERROR) != exp_pair)
			       || ((r_stub == NO_ERROR) != exp_stub);
		    if (r_entry == NO_ERROR && exp_entry)
		      {
			int ee = get_entry (bytes.data () + hs, loc, ow);
			if (ow == 2)
			  {
			    ee = (short) ee;
			  }
			bad = bad || (ee != e1);
		      }
		    if (bad)
		      {
			++mismatches;
		      }
		    o << loc << ":" << (r_entry == NO_ERROR) << (r_pair == NO_ERROR) << (r_stub == NO_ERROR)
		      << "/" << exp_entry << exp_pair << exp_stub << (bad ? "!" : "") << " ";
		  }
		return std::string (mismatches ? "ORACLE_MISMATCH " : "oracle_ok ") + o.str ();
	      } });
	    }
	}
    }
  std::vector<std::string> results = run_tasks (tasks);
  ASSERT_NO_FATAL_FAILURE (write_tsv ("DirectAccessorOracle", tasks, results));
  int bad = 0;
  for (size_t i = 0; i < tasks.size (); ++i)
    {
      if (results[i].compare (0, 15, "ORACLE_MISMATCH") == 0 || results[i].compare (0, 5, "CRASH") == 0
	  || results[i].compare (0, 4, "EXIT") == 0)
	{
	  if (bad++ < 20)
	    {
	      ADD_FAILURE () << tasks[i].name << " -> " << results[i];
	    }
	}
    }
  EXPECT_EQ (bad, 0);
}

// A1-5: valid records with every MVCC header size and offset width, read fresh and through one reused attr_info.
TEST_F (OosSqlA1VarOffsets, ValidHeaderSizesAndAttrInfoReuse)
{
  struct variant
  {
    int row;
    std::string name;
    std::vector<char> bytes;
  };
  std::vector<variant> variants;
  for (const row_ctx &row : rows)
    {
      for (int w :
	   {
		   0, 1, 2, 4
	   })
	{
	  std::vector<char> b = row.bytes;
	  if (w != 0 && !repack (row, w, b))
	    {
	      continue;
	    }
	  for (int mvcc = 0; mvcc < 8; ++mvcc)
	    {
	      variants.push_back ({ row.label, "w" + std::to_string (w) + "/mvcc" + std::to_string (mvcc),
				    with_mvcc_flags (b, mvcc) });
	    }
	}
    }
  // fresh results: one task per (variant, subset, api)
  std::vector<task> tasks;
  std::vector<std::string> ref_key;
  for (const variant &v : variants)
    {
      for (const subset_ctx &s : subsets)
	{
	  for (int api = 0; api < 3; ++api)
	    {
	      add_read (tasks, rows[v.row], v.name, v.bytes, (int) v.bytes.size (), 0, s, api);
	    }
	}
    }
  const size_t fresh_count = tasks.size ();
  // reuse results: one task per (subset, api), walking all variants in a shuffled but fixed order
  std::vector<size_t> order (variants.size ());
  for (size_t i = 0; i < order.size (); ++i)
    {
      order[i] = i;
    }
  for (size_t i = 0; i + 1 < order.size (); ++i)
    {
      std::swap (order[i], order[ (i * 7919 + 13) % order.size ()]);
    }
  for (const subset_ctx &s : subsets)
    {
      for (int api = 0; api < 3; ++api)
	{
	  OID coid = class_oid;
	  std::vector<variant> vs;
	  std::vector<OID> oids;
	  for (size_t i : order)
	    {
	      vs.push_back (variants[i]);
	      oids.push_back (rows[variants[i].row].oid);
	    }
	  tasks.push_back ({ "reuse/" + s.name + "/" + API_NAMES[api],
			     [vs, oids, s, api, coid] ()
	  {
	    THREAD_ENTRY *thread = thread_get_thread_entry_info ();
	    HEAP_CACHE_ATTRINFO attrs;
	    if (heap_attrinfo_start (thread, &coid, (int) s.ids.size (), const_cast<ATTR_ID *> (s.ids.data ()), &attrs) != NO_ERROR)
	      {
		return std::string ("start_failed");
	      }
	    std::string all;
	    for (size_t i = 0; i < vs.size (); ++i)
	      {
		all += vs[i].name + "@" + std::to_string (vs[i].row) + ": " + read_reuse (attrs, s, oids[i], vs[i].bytes,
		       (api_kind) api) + " ## ";
	      }
	    heap_attrinfo_end (thread, &attrs);
	    return all;
	  } });
	}
    }
  std::vector<std::string> results = run_tasks (tasks);
  ASSERT_NO_FATAL_FAILURE (write_tsv ("ValidHeaderSizesAndAttrInfoReuse", tasks, results));
  // fresh results must all succeed and every variant of a row must equal that row's stored-width/mvcc0 result
  std::map<std::string, std::string> canon; // (row, subset, api) -> result of first variant
  int bad = 0;
  size_t idx = 0;
  std::map<std::string, std::string> fresh; // (variant name@row, subset, api) -> result
  for (const variant &v : variants)
    {
      for (const subset_ctx &s : subsets)
	{
	  for (int api = 0; api < 3; ++api, ++idx)
	    {
	      const std::string &r = results[idx];
	      const std::string ck = std::to_string (v.row) + "/" + s.name + "/" + API_NAMES[api];
	      if (r.compare (0, 5, "ret=0") != 0)
		{
		  if (bad++ < 10)
		    {
		      ADD_FAILURE () << "fresh read failed: " << tasks[idx].name << " -> " << r;
		    }
		  continue;
		}
	      auto it = canon.find (ck);
	      if (it == canon.end ())
		{
		  canon[ck] = r;
		}
	      else if (it->second != r)
		{
		  if (bad++ < 10)
		    {
		      ADD_FAILURE () << "variant differs: " << tasks[idx].name << "\n  " << r << "\n  vs first " << it->second;
		    }
		}
	      fresh[v.name + "@" + std::to_string (v.row) + "/" + s.name + "/" + API_NAMES[api]] = r;
	    }
	}
    }
  // reused attr_info must reproduce the fresh result for every record in the walk
  for (size_t t = fresh_count; t < tasks.size (); ++t)
    {
      std::string name = tasks[t].name;      // reuse/<subset>/<api>
      std::string rest = name.substr (6);
      const std::string subset_api = rest;
      std::istringstream in (results[t]);
      std::string chunk;
      std::string text = results[t];
      size_t pos = 0;
      while (true)
	{
	  size_t e = text.find (" ## ", pos);
	  if (e == std::string::npos)
	    {
	      break;
	    }
	  std::string item = text.substr (pos, e - pos);
	  pos = e + 4;
	  size_t colon = item.find (": ");
	  if (colon == std::string::npos)
	    {
	      continue;
	    }
	  std::string key = item.substr (0, colon) + "/" + subset_api;
	  std::string got = item.substr (colon + 2);
	  auto it = fresh.find (key);
	  if (it == fresh.end () || it->second != got)
	    {
	      if (bad++ < 10)
		{
		  ADD_FAILURE () << "reused attr_info differs for " << key << "\n  reused: " << got << "\n  fresh:  "
				 << (it == fresh.end () ? std::string ("<none>") : it->second);
		}
	    }
	}
    }
  EXPECT_EQ (bad, 0);
}

// A1-6: deterministic random corruption near the header and variable offset table, plus random truncation.
TEST_F (OosSqlA1VarOffsets, RandomFuzz)
{
  std::vector<task> tasks;
  uint64_t state = 0x9E3779B97F4A7C15ULL;
  auto rnd = [&state] (uint32_t n)
  {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return (uint32_t) (state % n);
  };
  const int iterations = getenv ("A1_FUZZ_N") ? atoi (getenv ("A1_FUZZ_N")) : 4000;
  for (const row_ctx &row : rows)
    {
      for (int it = 0; it < iterations; ++it)
	{
	  int w = 0;
	  std::vector<char> bytes = row.bytes;
	  const int pick = rnd (4);
	  if (pick != 0)
	    {
	      static const int widths[] = { 0, 1, 2, 4 };
	      w = widths[pick];
	      if (!repack (row, w, bytes))
		{
		  bytes = row.bytes;
		  w = 0;
		}
	    }
	  const int window = std::min ((int) bytes.size (), (int) (row.header + OR_VAR_TABLE_SIZE_INTERNAL (row.nvar,
				       OR_GET_OFFSET_SIZE (bytes.data ())) + 8));
	  const int nmut = 1 + rnd (3);
	  std::ostringstream desc;
	  for (int m = 0; m < nmut; ++m)
	    {
	      int pos = rnd (window);
	      char val;
	      switch (rnd (4))
		{
		case 0:
		  val = (char) rnd (256);
		  break;
		case 1:
		  val = (char) (bytes[pos] ^ (1 << rnd (8)));
		  break;
		case 2:
		  val = (char) 0xFF;
		  break;
		default:
		  val = 0;
		}
	      bytes[pos] = val;
	      desc << "b" << pos << "=" << (int) (unsigned char) val << ",";
	    }
	  int length = (int) bytes.size ();
	  if (rnd (3) == 0)
	    {
	      length = 4 + (int) rnd ((uint32_t) bytes.size () - 3);
	    }
	  desc << "len" << length;
	  const subset_ctx &s = subsets[rnd ((uint32_t) subsets.size ())];
	  const int api = rnd (3);
	  const int placement = rnd (2);
	  add_read (tasks, row, "fuzz" + std::to_string (it) + "/w" + std::to_string (w) + "/" + desc.str (), bytes, length,
		    placement, s, api);
	}
    }
  std::vector<std::string> results = run_tasks (tasks);
  ASSERT_NO_FATAL_FAILURE (write_tsv ("RandomFuzz", tasks, results));
}

// A1-7: multi-column index keys built from variable and OOS columns (heap_midxkey_get_value), old and new representations.
TEST_F (OosSqlA1VarOffsets, MidxkeyIndexEquivalence)
{
  exec_sql ("DROP TABLE IF EXISTS t_a1m");
  db_commit_transaction ();
  ASSERT_GE (exec_sql ("CREATE TABLE t_a1m (id INT, v1 VARCHAR(100), b1 BIT VARYING, v2 VARCHAR(100), b2 BIT VARYING, "
		       "e1 VARCHAR(20), d1 DOUBLE)"), 0);
  ASSERT_GE (exec_sql ("INSERT INTO t_a1m VALUES "
		       "(1, 'alpha', X'0102', 'beta', X'ff', 'gamma', 1.5), "
		       "(2, 'x', CAST(REPEAT('AA', 4500) AS BIT VARYING), 'y', CAST(REPEAT('BB', 4700) AS BIT VARYING), 'z', 2.5), "
		       "(3, NULL, NULL, NULL, NULL, NULL, NULL), "
		       "(4, '', X'', '', X'', '', 4.5), "
		       "(5, 'one', CAST(REPEAT('CC', 4500) AS BIT VARYING), 'two', X'01', 'three', NULL), "
		       "(6, REPEAT('v', 90), CAST(REPEAT('DD', 4600) AS BIT VARYING), REPEAT('w', 90), "
		       "CAST(REPEAT('EE', 4650) AS BIT VARYING), REPEAT('e', 19), 6.5)"), 0);
  ASSERT_EQ (db_commit_transaction (), NO_ERROR);
  std::vector<std::string> lines;
  {
    THREAD_ENTRY *thread = thread_get_thread_entry_info ();
    DB_OBJECT *cls = db_find_class ("t_a1m");
    ASSERT_NE (cls, nullptr);
    OID class_oid_m = *WS_OID (cls);
    HFID hfid_m;
    ASSERT_EQ (heap_get_class_hfid (thread, &class_oid_m, &hfid_m, nullptr), NO_ERROR);
    DB_OBJLIST *objects = db_fetch_all_objects (cls, DB_FETCH_READ);
    ASSERT_NE (objects, nullptr);
    int with_oos = 0, total = 0;
    for (DB_OBJLIST *item = objects; item != nullptr; item = item->next)
      {
	OID oid = *WS_OID (item->op);
	HEAP_SCANCACHE scan;
	ASSERT_EQ (heap_scancache_start (thread, &scan, &hfid_m, &class_oid_m, true, logtb_get_mvcc_snapshot (thread)),
		   NO_ERROR);
	RECDES record = RECDES_INITIALIZER;
	const SCAN_CODE status = heap_get_visible_version (thread, &oid, &class_oid_m, &record, &scan, COPY, NULL_CHN,
				 HEAP_RECDES_DONT_CONSUME_RAW_BYTES);
	if (status == S_SUCCESS)
	  {
	    ++total;
	    with_oos += heap_recdes_contains_oos (&record) ? 1 : 0;
	  }
	heap_scancache_end (thread, &scan);
      }
    db_objlist_free (objects);
    lines.push_back ("rows=" + std::to_string (total) + " rows_with_oos=" + std::to_string (with_oos));
  }
  const char *ddl[] =
  {
    "CREATE INDEX i_a1m_1 ON t_a1m (v1, v2, e1)",
    "CREATE INDEX i_a1m_2 ON t_a1m (v2, b1, id)",
    "CREATE INDEX i_a1m_3 ON t_a1m (e1, b2, v1, d1)",
    "ALTER TABLE t_a1m ADD COLUMN added VARCHAR(20) DEFAULT 'dflt'",
    "INSERT INTO t_a1m VALUES (7, 'new', CAST(REPEAT('FF', 4500) AS BIT VARYING), 'n2', X'02', 'n4', 7.5, 'added7')",
    "CREATE INDEX i_a1m_4 ON t_a1m (added, v1, id)",
    "CREATE INDEX i_a1m_5 ON t_a1m (v2, added, b2)",
    "UPDATE t_a1m SET v1 = 'upd', b1 = b2 WHERE id IN (2, 4)",
    "DELETE FROM t_a1m WHERE id = 3"
  };
  for (const char *sql : ddl)
    {
      int rc = exec_sql (sql);
      lines.push_back (std::string ("ddl[") + sql + "]=" + std::to_string (rc < 0 ? rc : 0));
      db_commit_transaction ();
    }
  const char *queries[] =
  {
    "SELECT COUNT(*) FROM t_a1m",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_1) WHERE v1 = 'alpha' AND v2 = 'beta' AND e1 = 'gamma'",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_1) WHERE v1 IS NULL",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_1) WHERE v1 = ''",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_1) WHERE v1 = 'upd'",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_2) WHERE v2 = 'y' AND b1 = CAST(REPEAT('AA', 4500) AS BIT VARYING)",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_2) WHERE v2 = 'two' AND b1 = CAST(REPEAT('CC', 4500) AS BIT VARYING) AND id = 5",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_2) WHERE v2 = 'beta'",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_3) WHERE e1 = 'z' AND b2 = CAST(REPEAT('BB', 4700) AS BIT VARYING)",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_3) WHERE e1 = 'e' AND b2 = X'01'",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_4) WHERE added = 'dflt' AND v1 = 'x' AND id = 2",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_4) WHERE added = 'dflt'",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_4) WHERE added = 'added7' AND v1 = 'new'",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_5) WHERE v2 = 'two' AND added = 'dflt' AND b2 = X'01'",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_5) WHERE v2 = 'n2' AND added = 'added7'",
    "SELECT /*+ RECOMPILE */ COUNT(*) FROM t_a1m USE INDEX (i_a1m_5) WHERE v2 = ''",
    "SELECT COUNT(*) FROM t_a1m WHERE b1 = b2"
  };
  for (const char *sql : queries)
    {
      int count = -1;
      int rc = fetch_single_int (sql, &count);
      lines.push_back (std::string ("q[") + sql + "]=" + std::to_string (rc) + ":" + std::to_string (count));
    }
  const char *dir = getenv ("A1_OUT");
  std::string d = dir ? dir : "./a1_out";
  mkdir (d.c_str (), 0755);
  FILE *f = fopen ((d + "/MidxkeyIndexEquivalence.tsv").c_str (), "w");
  ASSERT_NE (f, nullptr);
  for (const std::string &l : lines)
    {
      fprintf (f, "%s\n", l.c_str ());
    }
  fclose (f);
  exec_sql ("DROP TABLE IF EXISTS t_a1m");
  db_commit_transaction ();
}

// A1-8: one attr_info reused for a valid record followed by a record shorter than its header / VOT.
TEST_F (OosSqlA1VarOffsets, ReuseThenShortRecord)
{
  std::vector<task> tasks;
  for (const row_ctx &row : rows)
    {
      for (const subset_ctx &s : subsets)
	{
	  if (s.name != "ALL" && s.name != "VARS" && s.name != "OOSPAIR" && s.name != "only_e1")
	    {
	      continue;
	    }
	  for (int api = 0; api < 3; ++api)
	    {
	      for (int len = 4; len <= row.header + 4 * row.width + 12; ++len)
		{
		  OID coid = class_oid;
		  OID oid = row.oid;
		  std::vector<char> full = row.bytes;
		  tasks.push_back ({ tag (row, "validThenTrunc" + std::to_string (len), s, api, 0),
				     [full, len, s, api, coid, oid] ()
		  {
		    THREAD_ENTRY *thread = thread_get_thread_entry_info ();
		    HEAP_CACHE_ATTRINFO attrs;
		    if (heap_attrinfo_start (thread, &coid, (int) s.ids.size (), const_cast<ATTR_ID *> (s.ids.data ()), &attrs) != NO_ERROR)
		      {
			return std::string ("start_failed");
		      }
		    std::string a = read_reuse (attrs, s, oid, full, (api_kind) api);
		    std::vector<char> shortrec (full.begin (), full.begin () + len);
		    std::string b = read_reuse (attrs, s, oid, shortrec, (api_kind) api);
		    heap_attrinfo_end (thread, &attrs);
		    return "first=" + a.substr (0, 6) + " second=" + b;
		  } });
		}
	    }
	}
    }
  std::vector<std::string> results = run_tasks (tasks);
  ASSERT_NO_FATAL_FAILURE (write_tsv ("ReuseThenShortRecord", tasks, results));
}

int
main (int argc, char **argv)
{
  ::testing::InitGoogleTest (&argc, argv);
  ::testing::AddGlobalTestEnvironment (new SqlServerEnv ());
  return RUN_ALL_TESTS ();
}
