#
# Copyright (c) 2021 Intel Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

FROM registry.access.redhat.com/ubi8/ubi:8.8 as rpmbuild

WORKDIR /root
COPY rpmbuild.sh /root/
RUN /root/rpmbuild.sh

FROM registry.access.redhat.com/ubi8/ubi:8.8

RUN mkdir /rpmbuild/
COPY --from=rpmbuild /ovms-rpmbuild-deps.tar.xz /rpmbuild/
COPY get.sh /
RUN /get.sh

ENTRYPOINT [ "sleep", "360" ]
