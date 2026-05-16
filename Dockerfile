FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Use a faster mirror and update
RUN sed -i 's/archive.ubuntu.com/mirror.arizona.edu/g' /etc/apt/sources.list && \
    apt-get update

# Install packages in smaller groups to see progress
RUN apt-get install -y build-essential cmake gdb

RUN apt-get install -y libsfml-dev fonts-dejavu-core

RUN apt-get install -y libsdl2-dev libglfw3-dev

RUN apt-get install -y libncurses-dev

RUN rm -rf /var/lib/apt/lists/*

COPY requirements.txt /tmp/requirements.txt
RUN grep -v '^#' /tmp/requirements.txt | grep -v '^$' | \
    xargs -r apt-get install -y && rm -rf /var/lib/apt/lists/*

WORKDIR /app

CMD ["bash"]
