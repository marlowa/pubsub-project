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
    && alternatives --set python3 /usr/bin/python3.8 \
    && dnf clean all

# Configure git to allow /workspace
RUN git config --global --add safe.directory /workspace

WORKDIR /workspace
CMD ["/bin/bash"]
