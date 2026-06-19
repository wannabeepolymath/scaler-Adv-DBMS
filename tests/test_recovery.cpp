// Crash recovery: committed transactions survive a crash via WAL redo;
// uncommitted ones do not.
#include <cstdio>
#include <string>

#include "engine/database.h"
#include "tests/test_util.h"

using namespace minidb;

static void TestCrashRecovery() {
  const std::string f = "test_recovery.db";
  const std::string wal = f + ".wal";
  std::remove(f.c_str());
  std::remove(wal.c_str());

  // Session 1: commit data through several paths, then pull the plug.
  {
    Database db(f);
    db.Execute("CREATE TABLE t (id INT, name VARCHAR(16))");
    db.Execute("CREATE INDEX t_id ON t (id)");
    db.Execute("INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c')");  // auto-commit
    db.Execute("BEGIN");
    db.Execute("INSERT INTO t VALUES (4,'d')");
    db.Execute("COMMIT");  // explicit committed transaction
    db.Execute("BEGIN");
    db.Execute("INSERT INTO t VALUES (5,'e')");  // left uncommitted at crash
    db.SimulateCrash();
  }

  // Session 2: reopen triggers WAL redo of committed records only.
  {
    Database db(f);
    CHECK_EQ(db.Execute("SELECT id FROM t").affected, 4);               // 1..4 survived
    CHECK_EQ(db.Execute("SELECT name FROM t WHERE id = 4").affected, 1);  // committed txn redone
    CHECK_EQ(db.Execute("SELECT id FROM t WHERE id = 5").affected, 0);    // uncommitted lost
    // The index is rebuilt during recovery, so point lookups still work.
    ResultSet r = db.Execute("SELECT name FROM t WHERE id = 2");
    CHECK_EQ(r.affected, 1);
    CHECK(r.rows.size() == 1 && r.rows[0][0].Equals(Value(std::string("b"))));
  }

  // Session 3: a clean shutdown happened (session 2 closed normally), so reopening
  // must NOT replay again — data is read straight from the flushed pages.
  {
    Database db(f);
    CHECK_EQ(db.Execute("SELECT id FROM t").affected, 4);
  }

  std::remove(f.c_str());
  std::remove(wal.c_str());
}

// A committed delete is also redone on recovery (the row stays gone).
static void TestRecoveryWithDelete() {
  const std::string f = "test_recovery_del.db";
  const std::string wal = f + ".wal";
  std::remove(f.c_str());
  std::remove(wal.c_str());
  {
    Database db(f);
    db.Execute("CREATE TABLE t (id INT, v INT)");
    db.Execute("INSERT INTO t VALUES (1,10),(2,20),(3,30)");
    db.Execute("DELETE FROM t WHERE id = 2");  // committed delete
    db.SimulateCrash();
  }
  {
    Database db(f);
    CHECK_EQ(db.Execute("SELECT id FROM t").affected, 2);             // 1 and 3 remain
    CHECK_EQ(db.Execute("SELECT id FROM t WHERE id = 2").affected, 0);  // delete redone
  }
  std::remove(f.c_str());
  std::remove(wal.c_str());
}

int main() {
  TestCrashRecovery();
  TestRecoveryWithDelete();
  return minidb::test::summary("recovery");
}
