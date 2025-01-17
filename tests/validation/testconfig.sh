#
# src/test/testconfig.sh -- configuration for local and remote unit tests
#

#
# 1) *** LOCAL CONFIGURATION ***
#
# The first part of the file tells the script unittest/unittest.sh
# which file system locations are to be used for local testing.
#

#
# Appended to PMEM_FS_DIR and NON_PMEM_FS_DIR to test PMDK with
# file path longer than 255 characters.
#
# LONGDIR="LoremipsumdolorsitametconsecteturadipiscingelitVivamuslacinianibhattortordictumsollicitudinNullamvariusvestibulumligulaetegestaselitsemperidMaurisultriciesligulaeuipsumtinciduntluctusMorbimaximusvariusdolorid"
# DIRSUFFIX="$LONGDIR/$LONGDIR/$LONGDIR/$LONGDIR/$LONGDIR"
#
# For tests that require true persistent memory, set the path to a directory
# on a PMEM-aware file system here.  Comment this line out if there's no
# actual persistent memory available on this system.
# Note that PMEM_FS_DIR is intended mostly to test codepaths where
# pmem_persist() is used for flushing data to persistence.  For now, there is
# no file system on Linux that fully supports pmem_persist(), so even in case
# of DAX-enabled file systems (like ext4), PMDK would behave as for non-PMEM
# file system and would still use pmem_msync().
# You may change this behavior by setting PMEM_FS_DIR_FORCE_PMEM (see below).
# To fully test the PMEM codepaths it is strongly recommended to configure
# DEVICE_DAX_PATH as well.
#
PMEM_FS_DIR=/tmp/$(whoami)

#
# For tests that require true a non-persistent memory aware file system (i.e.
# to verify something works on traditional page-cache based memory-mapped
# files) set the path to a directory on a normal file system here.
#
NON_PMEM_FS_DIR=/tmp/$(whoami)

#
# If you don't have real PMEM or PMEM emulation set up and/or the filesystem
# does not support MAP_SYNC flag, but still want to test PMEM codepaths
# uncomment this line. It will set PMEM_IS_PMEM_FORCE to 1 for tests that
# require pmem.
#
# Setting this flag to 1, if the PMEM_FS_DIR filesystem supports MAP_SYNC will
# cause an error. This flag cannot be used with filesystems which support
# MAP_SYNC because it would prevent from testing the target PMEM codepaths.
# If you want to ignore this error set the value to 2.
#
PMEM_FS_DIR_FORCE_PMEM=1

#
# For tests that require raw dax devices without a file system, set a path to
# those devices in an array format. For most tests one device is enough, but
# some might require more.
#
#DEVICE_DAX_PATH=(/dev/dax0.0)

#
# Overwrite default test type:
# check (default), short, medium, long, all
# where: check = short + medium; all = short + medium + long
#
TEST_TYPE=all

#
# Overwrite available build types:
# debug, nondebug, static-debug, static-nondebug, all (default)
#
# TEST_BUILD=static-nondebug

#
# Overwrite available filesystem types:
# pmem, non-pmem, any, none, all (default)
#
TEST_FS=pmem

#
# Overwrite default timeout
# (floating point number with an optional suffix: 's' for seconds (the default),
# 'm' for minutes, 'h' for hours or 'd' for days)
#
TEST_TIMEOUT=20m

#
# To display execution time of each test
#
TM=1

#
# Normally the first failed test terminates the test run. If KEEP_GOING
# is set, continues executing all tests. If any tests fail, once all tests
# have completed reports number of failures, lists failed tests and exits
# with error status.
#
KEEP_GOING=y

#
# This option works only if KEEP_GOING=y, then if CLEAN_FAILED is set
# all data created by test is removed on test failure.
#
#CLEAN_FAILED=y

#
# Changes logging level. Possible values:
# 0 - silent (only error messages)
# 1 - normal (above + SETUP + START + DONE + PASS + important SKIP messages)
# 2 - verbose (above + all SKIP messages + stdout from test binaries)
#
UNITTEST_LOG_LEVEL=2

#
# Test against installed libraries, NOT the one built in tree.
# Note that these variable won't affect tests that link statically. You should
# disabled them using TEST_BUILD variable.
#
#PMDK_LIB_PATH_NONDEBUG=/usr/lib/x86_64-linux-gnu/
#PMDK_LIB_PATH_DEBUG=/usr/lib/x86_64-linux-gnu/pmdk_dbg

#
# The 'nfit' tests test the code handling bad blocks.
# They use the 'sudo' command many times and insert the 'nfit_test' kernel
# module, so they can be considered as POTENTIALLY DANGEROUS
# and have to be explicitly enabled.
# Enable them ONLY IF you are sure you know what you are doing.
#
#ENABLE_NFIT_TESTS=y

#
# 2) *** REMOTE CONFIGURATION ***
#
# The second part of the file tells the script unittest/unittest.sh
# which remote nodes and their file system locations are to be used
# for remote testing.
#

#
# Addresses of nodes should be defined as an array:
#
#    NODE[index]=[<user>@]<host-name-or-IP>
#
# The remote account must be set up for automated ssh authentication.
# The remote user's login shell must be bash.
#
#NODE[0]=127.0.0.1
#NODE[1]=user1@host1
#NODE[2]=user2@host2

#
# Addresses of interfaces which the remote nodes
# shall communicate on should be defined as an array:
#
#    NODE_ADDR[index]=[<user>@]<host-name-or-IP>
#
#NODE_ADDR[0]=192.168.0.1
#NODE_ADDR[1]=192.168.0.2
#NODE_ADDR[2]=192.168.0.3

#
# Working directories on remote nodes (they will be created)
#
#NODE_WORKING_DIR[0]=/remote/dir0
#NODE_WORKING_DIR[1]=/remote/dir1
#NODE_WORKING_DIR[2]=/remote/dir2

#
# NODE_LD_LIBRARY_PATH variable for each remote node
#
#NODE_LD_LIBRARY_PATH[0]=/usr/local/lib
#NODE_LD_LIBRARY_PATH[1]=/usr/local/lib
#NODE_LD_LIBRARY_PATH[2]=/usr/local/lib
#

#
# NODE_DEVICE_DAX_PATH variable for each remote node which
# can be used to specify path to multiple device daxes on remote node.
#
# All remote tests assume the master replica is present on a node of index 1.
# So the size of device dax on the node of index 1 should not be bigger than
# a size of device dax devices on other nodes.
#
#NODE_DEVICE_DAX_PATH[0]="/dev/dax0.0 /dev/dax1.0"
#NODE_DEVICE_DAX_PATH[1]="/dev/dax0.0 /dev/dax1.0"
#NODE_DEVICE_DAX_PATH[2]="/dev/dax0.0"
#NODE_DEVICE_DAX_PATH[3]="/dev/dax0.0"

#
# NODE_ENV variable for setting environment variables on specified nodes
#
#NODE_ENV[0]="VAR=1"
#NODE_ENV[1]="VAR=\$VAR:1"
#NODE_ENV[2]=""

#
# RPMEM_VALGRIND_ENABLED variable enables valgrind rpmem tests
# Valgrind rpmem tests require libibverbs and librdmacm compiled with valgrind
# support.
#
#RPMEM_VALGRIND_ENABLED=y

#
# Overwrite available providers:
# verbs, sockets, all (default)
#
#TEST_PROVIDERS=all

#
# Overwrite available persistency methods:
# GPSPM, APM, all (default)
#
#TEST_PMETHODS=all
