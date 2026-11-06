#!/bin/sh

test_description='Test MPI-4 Sessions over the pmix shell plugin.

A Sessions program builds a communicator from the WORLD process set,
from an even-rank subset, and from the odd ranks in descending order.
MPICH >= 5.0 bootstraps each subset communicator with a PMIx fence over
the ranks of its node roots when it is told the process manager supports
one, and with whole-job fences otherwise.  Open MPI 5.0 builds them on
PMIx groups, which the server does not provide yet.

MPICH lists the node roots in communicator rank order, so the descending
subset fences over a descending rank list.  The plugin maps the list to
the set of shells that host those ranks, so order is immaterial to it,
unlike the PMI-1.2 group barrier (flux-core issue #7888).  The test
covers it anyway for parity with flux-core.

Two MPICH details decide whether it issues the subset fence at all:
 - MPICH main after v5.0.2 defaults MPIR_CVAR_PMI_DISABLE_GROUP to 1.
   The test sets it to 0.
 - The ch3 device defines MPID_SESSION_USE_WORLD, so it bootstraps the
   subset communicator through MPI_COMM_WORLD and issues only whole-job
   fences.  Only ch4 issues the subset fence.
The test therefore asserts on the whole-job fences, which every MPICH
sends, and only reports whether the subset fences were seen.
'

. `dirname $0`/sharness.sh

MPI_VERSION=${FLUX_BUILD_DIR}/t/src/mpi_version
SESSIONS=${FLUX_BUILD_DIR}/t/src/mpi_sessions

export FLUX_SHELL_RC_PATH=${FLUX_BUILD_DIR}/t/etc

if ! ${MPI_VERSION} >/dev/null 2>&1; then
    skip_all='skipping MPI Sessions tests - MPI unavailable'
    test_done
fi
if test $(${MPI_VERSION} --flavor) != mpich; then
    skip_all='skipping MPI Sessions tests - MPI is not MPICH'
    test_done
fi
if ! ${SESSIONS} --probe; then
    skip_all='skipping MPI Sessions tests - MPI-4 Sessions not supported'
    test_done
fi

export MPIR_CVAR_PMI_DISABLE_GROUP=0

test_under_flux 2

test_expect_success 'MPI Sessions job runs on one node (n=2)' '
	run_timeout 60 flux run -N1 -n2 ${SESSIONS} >one.out &&
	grep "MPI Sessions test OK: size=2" one.out
'

test_expect_success 'MPI Sessions job runs across nodes (n=4)' '
	run_timeout 60 flux run -N2 -n4 ${SESSIONS} >two.out &&
	grep "MPI Sessions test OK: size=4" two.out
'

# The WORLD communicator exchanges business cards in a collecting fence
# over the whole job.  Every shell runs that exchange.
test_expect_success 'the WORLD bootstrap is a whole-job fence on each shell' '
	run_timeout 60 flux run -N2 -n4 -overbose=2 ${SESSIONS} \
		2>trace.err >trace.out &&
	grep "MPI Sessions test OK: size=4" trace.out &&
	grep "flux-shell\[0\].*completed pmix exchange 0 (all:0)" trace.err &&
	grep "flux-shell\[1\].*completed pmix exchange 0 (all:0)" trace.err
'

# ch4 fences over the node roots of each subset.  With -N2 -n4 and block
# placement, the even subset {0,2} has node roots 0 and 2 and the
# descending odd subset (3,1) has node roots 3 and 1, so each exchange
# is again among all shells but under a tag of its own.  ch3 never gets
# here, so report rather than assert, but if one subset fenced then both
# must have.
test_expect_success 'report whether the subset bootstraps fenced' '
	if grep -q "completed pmix exchange 1 (all:1)" trace.err; then
		echo "# subset communicators bootstrapped with fences (ch4)" &&
		grep "flux-shell\[0\].*completed pmix exchange 1 (all:1)" trace.err &&
		grep "flux-shell\[1\].*completed pmix exchange 1 (all:1)" trace.err &&
		grep "flux-shell\[0\].*completed pmix exchange 2 (all:2)" trace.err &&
		grep "flux-shell\[1\].*completed pmix exchange 2 (all:2)" trace.err
	else
		echo "# subset communicators bootstrapped through WORLD (ch3)"
	fi
'

test_done
