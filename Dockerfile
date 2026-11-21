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
    zlib-devel \
    libzstd-devel \
    libcurl-devel \
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
# pybind11 round-trip tests to configure, build, and load. backports.zoneinfo gives
# Python 3.8 the time zones that scripts/operator_requests.py reads a day's schedule
# with; Python has them itself from 3.9.
RUN dnf install -y python3-pip python38-devel \
    && dnf clean all \
    && pip3 install pytest pylint tomli gcovr pybind11 backports.zoneinfo

# Liquibase — copied from the host installation in docker-deps/ (no internet required)
COPY docker-deps/liquibase /opt/liquibase
RUN ln -s /opt/liquibase/liquibase /usr/bin/liquibase

# Doxygen and graphviz for the documentation build. Neither is in BaseOS,
# AppStream or EPEL: on Rocky 8 doxygen lives in the powertools repo, which is
# disabled by default (the RHEL8 equivalent is codeready-builder-for-rhel-8-*-rpms).
# The version this pulls is 1.8.14, which is the whole point -- it is the version
# the real RHEL8 build hosts have, and the one the Doxyfile is written for. Without
# it this container cannot validate the docs build on the target toolchain, which is
# what the container exists to do.
RUN dnf install -y --enablerepo=powertools doxygen graphviz \
    && dnf clean all

# No TeX. The book in docs/book is built only on a machine with a current TeX Live, such as TeX Live 2023: Rocky 8
# provides no package holding glossaries, makeglossaries, cleveref, tex4ebook or make4ht. docs/book/Makefile checks
# for them first and says which are missing.

# TLA+ tools (the TLC model checker), used by scripts/tla_trace_pages.py to reproduce the
# counterexamples of docs/availability/tla/MajorityLeaseHA.tla at install time. Not packaged
# for Rocky 8: it is a single jar, tla2tools.jar, and runs on the Java 17 installed above.
#
# The jar is not downloaded. It is copied from the directory that holds the copy on the machine
# building the image, which docker build is given as a second build input named tla2tools:
#
#   docker build -t pubsub-rhel8 --build-context tla2tools=$THIRDPARTY_DIR/tla2tools-1.8.0 .
#
# Release 1.8.0 on the TLA+ project's GitHub releases page is a pre-release, which that project
# rebuilds and uploads again under the same name, so the file it serves changes from time to time
# and a checksum of it does not stay true. The checksum below is that of the build of 30 October 2026
# (tlaplus revision 090e85c), the one every model in this project has been checked with. The check
# makes sure the image holds that build and no other; a different file stops the image build here.
# Without the --build-context argument, docker build stops at the COPY line, because it then looks
# for an image named tla2tools.
ARG TLA_TOOLS_VERSION=1.8.0
ARG TLA_TOOLS_SHA256=011267fca25d0752cf18894dd585cd28b96960c112ea8a20b823ea770e7e10cd
COPY --from=tla2tools tla2tools.jar /opt/tla2tools-${TLA_TOOLS_VERSION}/tla2tools.jar
RUN echo "${TLA_TOOLS_SHA256}  /opt/tla2tools-${TLA_TOOLS_VERSION}/tla2tools.jar" | sha256sum -c -
ENV TLA2TOOLS_JAR=/opt/tla2tools-1.8.0/tla2tools.jar

# Configure git to allow /workspace
RUN git config --global --add safe.directory /workspace

COPY scripts/docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

WORKDIR /workspace
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
CMD ["/bin/bash"]
