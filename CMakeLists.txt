###########################################################################
# Copyright (C) 2015-2024 IoT.bzh Company
#
# Author: José Bollo <jose.bollo@iot.bzh>
#
# $RP_BEGIN_LICENSE$
# Commercial License Usage
#  Licensees holding valid commercial IoT.bzh licenses may use this file in
#  accordance with the commercial license agreement provided with the
#  Software or, alternatively, in accordance with the terms contained in
#  a written agreement between you and The IoT.bzh Company. For licensing terms
#  and conditions see https://www.iot.bzh/terms-conditions. For further
#  information use the contact form at https://www.iot.bzh/contact.
# 
# GNU General Public License Usage
#  Alternatively, this file may be used under the terms of the GNU General
#  Public license version 3. This license is as published by the Free Software
#  Foundation and appearing in the file LICENSE.GPLv3 included in the packaging
#  of this file. Please review the following information to ensure the GNU
#  General Public License requirements will be met
#  https://www.gnu.org/licenses/gpl-3.0.html.
# $RP_END_LICENSE$
###########################################################################

CMAKE_MINIMUM_REQUIRED(VERSION 3.10)

project(afb-client
        VERSION 4.2.1
        DESCRIPTION "Basic Client of Application Framework Binder"
        HOMEPAGE_URL "https://github.com/redpesk-core/afb-client"
        LANGUAGES C)

INCLUDE(FindPkgConfig)
INCLUDE(GNUInstallDirs)

pkg_check_modules(modules REQUIRED json-c libsystemd>=222 libafbcli>=5.3.3)
pkg_check_modules(readline readline)
if(readline_FOUND)
	add_compile_options(-DWITH_READLINE=1)
endif(readline_FOUND)

add_subdirectory(src)
add_subdirectory(docs)

