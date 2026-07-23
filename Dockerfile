FROM rockylinux:8

# System build tools
RUN dnf install -y \
    gcc \
    gcc-c++ \
    git \
    cmake \
    make \
    python38 \
    findutils \
    wget \
    tar \
    which \
    perl \
    libatomic \
    numactl-devel \
    sudo \
    openssl \
    openssl-devel \
    epel-release \
    && dnf install -y lcov \
    && alternatives --set python3 /usr/bin/python3.8 \
    && dnf clean all

# PostgreSQL — use the version native to Rocky 8 (matches RHEL8 target environment).
# initdb is done at runtime by docker-entrypoint.sh, not here.
RUN dnf install -y postgresql-server postgresql \
    && dnf clean all

# Java 17 JDK — discover the versioned path and create a stable symlink for JAVA_HOME
RUN dnf install -y java-17-openjdk-devel \
    && dnf clean all \
    && java_home=$(dirname $(dirname $(readlink -f /usr/bin/java))) \
    && ln -sfn "${java_home}" /usr/lib/jvm/java-17-current
ENV JAVA_HOME=/usr/lib/jvm/java-17-current
ENV PATH="${JAVA_HOME}/bin:${PATH}"

# Maven — use the version from the Rocky 8 repos (3.5.x) to match the RHEL8 target environment
RUN dnf install -y maven \
    && dnf clean all

# Python tooling for the DSL test suite and linting. python38-devel supplies
# Python.h (needed to *compile* the generated extension) and the pip pybind11
# supplies the module plus its cmake dir (the DSL test harness does
# `import pybind11` and feeds cmake -Dpybind11_DIR); both are required for the
# pybind11 round-trip tests to configure, build, and load.
RUN dnf install -y python3-pip python38-devel \
    && dnf clean all \
    && pip3 install pytest pylint tomli gcovr pybind11

# Liquibase — copied from the host installation in docker-deps/ (no internet required)
COPY docker-deps/liquibase /opt/liquibase
RUN ln -s /opt/liquibase/liquibase /usr/bin/liquibase

# Configure git to allow /workspace
RUN git config --global --add safe.directory /workspace

COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

WORKDIR /workspace
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["/bin/bash"]
