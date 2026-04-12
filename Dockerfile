FROM rockylinux:8

# System build tools
RUN dnf install -y \
    gcc \
    gcc-c++ \
    git \
    cmake \
    make \
    python3 \
    boost-devel \
    boost-filesystem \
    boost-system \
    findutils \
    which \
    wget \
    && dnf clean all

# Configure git to allow /workspace
RUN git config --global --add safe.directory /workspace

WORKDIR /workspace
CMD ["/bin/bash"]
