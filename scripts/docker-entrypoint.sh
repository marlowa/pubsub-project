#!/usr/bin/env bash
set -euo pipefail

PGDATA=/var/lib/pgsql/data

# When a Docker named volume is mounted at PGDATA the directory starts
# root-owned and empty.  Ensure the postgres user owns it before touching it.
chown postgres:postgres "${PGDATA}"

# Initialise the cluster if not already done (fresh volume on first run).
if [ ! -f "${PGDATA}/PG_VERSION" ]; then
    su -s /bin/bash postgres -c "initdb -D ${PGDATA}"
fi

# Start PostgreSQL as the postgres unix user.
su -s /bin/bash postgres -c \
    "pg_ctl start -D ${PGDATA} -l /var/lib/pgsql/startup.log"

# Wait up to 15 seconds for the server to accept connections.
for i in $(seq 1 15); do
    su -s /bin/bash postgres -c "pg_isready -q" 2>/dev/null && break
    sleep 1
done

exec "$@"
