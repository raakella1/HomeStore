# ##########   #######   ############
FROM ecr.vip.ebayc3.com/sds/sds_cpp_base:2.14
LABEL description="Automated SDS compilation"

ARG BUILD_TYPE
ARG CONAN_CHANNEL
ARG CONAN_USER
ARG CONAN_PASS=${CONAN_USER}
ARG HOMESTORE_BUILD_TAG
ENV BUILD_TYPE=${BUILD_TYPE:-default}
ENV CONAN_USER=${CONAN_USER:-sds}
ENV CONAN_CHANNEL=${CONAN_CHANNEL:-develop}
ENV CONAN_PASS=${CONAN_PASS:-password}
ENV HOMESTORE_BUILD_TAG=${HOMESTORE_BUILD_TAG:-release}
ENV SOURCE_PATH=/tmp/source/

COPY .git/ ${SOURCE_PATH}.git
RUN cd ${SOURCE_PATH}; git reset --hard

WORKDIR /output
ENV ASAN_OPTIONS=detect_leaks=0

RUN set -eux; \
    if [ "nosanitize" = "${BUILD_TYPE}" ]; then \
      eval $(grep 'name =' ${SOURCE_PATH}conanfile.py | sed 's, ,,g' | sed 's,name,PKG_NAME,'); \
      eval $(grep -m 1 'version =' ${SOURCE_PATH}conanfile.py | sed 's, ,,g' | sed 's,version,PKG_VERSION,'); \
      conan install --build missing -o ${PKG_NAME}:coverage=True -pr ${BUILD_TYPE} ${SOURCE_PATH}; \
      /usr/local/bin/build-wrapper-linux-x86-64 --out-dir /tmp/sonar conan build ${SOURCE_PATH}; \
      cp ${SOURCE_PATH}sonar-project.properties ./; \
      find . -name "*.gcno" -exec gcov {} \; ; \
      /usr/local/bin/sonar-scanner -Dsonar.projectBaseDir=${SOURCE_PATH} -Dsonar.projectVersion="${PKG_VERSION}"; \
    fi;

RUN conan create -pr ${BUILD_TYPE} ${SOURCE_PATH} "${CONAN_USER}"/"${CONAN_CHANNEL}"

CMD set -eux; \
    eval $(grep 'name =' ${SOURCE_PATH}conanfile.py | sed 's, ,,g' | sed 's,name,PKG_NAME,'); \
    eval $(grep 'version =' ${SOURCE_PATH}conanfile.py | sed 's, ,,g' | sed 's,version,PKG_VERSION,'); \
    conan user -r ebay-sds -p "${CONAN_PASS}" sds; \
    conan upload ${PKG_NAME}/"${PKG_VERSION}"@"${CONAN_USER}"/"${CONAN_CHANNEL}" --all -r ebay-sds;
# ##########   #######   ############
