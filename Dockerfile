FROM rockylinux:8

# System build tools
RUN dnf install -y \
    gcc \
    gcc-c++ \
    git \
    cmake \
    make \
    python38 \
    boost-devel \
    boost-filesystem \
    boost-system \
    findutils \
    wget \
    tar \
    which \
    perl \
    libatomic \
    numactl-devel \
    sudo \
    openssl \
    && alternatives --set python3 /usr/bin/python3.8 \
    && dnf clean all

# PostgreSQL 13 — needed for create_db.py / deploy.py
RUN dnf module enable postgresql:13 -y \
    && dnf install -y postgresql-server postgresql \
    && dnf clean all \
    && postgresql-setup --initdb

# Configure git to allow /workspace
RUN git config --global --add safe.directory /workspace

COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

WORKDIR /workspace
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["/bin/bash"]
