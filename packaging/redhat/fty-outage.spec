#
#    fty-outage - Agent that sends alerts when device does not communicate.
#
#    Copyright (C) 2014 - 2015 Eaton                                        
#                                                                           
#    This program is free software; you can redistribute it and/or modify   
#    it under the terms of the GNU General Public License as published by   
#    the Free Software Foundation; either version 2 of the License, or      
#    (at your option) any later version.                                    
#                                                                           
#    This program is distributed in the hope that it will be useful,        
#    but WITHOUT ANY WARRANTY; without even the implied warranty of         
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          
#    GNU General Public License for more details.                           
#                                                                           
#    You should have received a copy of the GNU General Public License along
#    with this program; if not, write to the Free Software Foundation, Inc.,
#    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.            
#

# To build with draft APIs, use "--with drafts" in rpmbuild for local builds or add
#   Macros:
#   %_with_drafts 1
# at the BOTTOM of the OBS prjconf
%bcond_with drafts
%if %{with drafts}
%define DRAFTS yes
%else
%define DRAFTS no
%endif
Name:           fty-outage
Version:        1.0.0
Release:        1
Summary:        agent that sends alerts when device does not communicate.
License:        GPL-2.0+
URL:            https://42ity.com/
Source0:        %{name}-%{version}.tar.gz
Group:          System/Libraries
# Note: ghostscript is required by graphviz which is required by
#       asciidoc. On Fedora 24 the ghostscript dependencies cannot
#       be resolved automatically. Thus add working dependency here!
BuildRequires:  ghostscript
BuildRequires:  asciidoc
BuildRequires:  automake
BuildRequires:  autoconf
BuildRequires:  libtool
BuildRequires:  pkgconfig
BuildRequires:  systemd-devel
BuildRequires:  systemd
%{?systemd_requires}
BuildRequires:  xmlto
BuildRequires:  zeromq-devel
BuildRequires:  czmq-devel
BuildRequires:  malamute-devel
BuildRequires:  fty-proto-devel
BuildRoot:      %{_tmppath}/%{name}-%{version}-build

%description
fty-outage agent that sends alerts when device does not communicate..

%package -n libfty_outage1
Group:          System/Libraries
Summary:        agent that sends alerts when device does not communicate. shared library

%description -n libfty_outage1
This package contains shared library for fty-outage: agent that sends alerts when device does not communicate.

%post -n libfty_outage1 -p /sbin/ldconfig
%postun -n libfty_outage1 -p /sbin/ldconfig

%files -n libfty_outage1
%defattr(-,root,root)
%{_libdir}/libfty_outage.so.*

%package devel
Summary:        agent that sends alerts when device does not communicate.
Group:          System/Libraries
Requires:       libfty_outage1 = %{version}
Requires:       zeromq-devel
Requires:       czmq-devel
Requires:       malamute-devel
Requires:       fty-proto-devel

%description devel
agent that sends alerts when device does not communicate. development tools
This package contains development files for fty-outage: agent that sends alerts when device does not communicate.

%files devel
%defattr(-,root,root)
%{_includedir}/*
%{_libdir}/libfty_outage.so
%{_libdir}/pkgconfig/libfty_outage.pc
%{_mandir}/man3/*

%prep
%setup -q

%build
sh autogen.sh
%{configure} --enable-drafts=%{DRAFTS} --with-systemd-units
make %{_smp_mflags}

%install
make install DESTDIR=%{buildroot} %{?_smp_mflags}

# remove static libraries
find %{buildroot} -name '*.a' | xargs rm -f
find %{buildroot} -name '*.la' | xargs rm -f

%files
%defattr(-,root,root)
%{_bindir}/fty-outage
%{_mandir}/man1/fty-outage*
%config(noreplace) %{_sysconfdir}/fty-outage/fty-outage.cfg
/usr/lib/systemd/system/fty-outage.service
%dir %{_sysconfdir}/fty-outage
%if 0%{?suse_version} > 1315
%post
%systemd_post fty-outage.service
%preun
%systemd_preun fty-outage.service
%postun
%systemd_postun_with_restart fty-outage.service
%endif

%changelog
