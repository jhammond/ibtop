Name: ibtop
Version: 1.0.0
Release: 1
License: GPL
Vendor: TACC/Ranger
Group: System Environment/Base
Packager: TACC - jhammond@tacc.utexas.edu
Source: %{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root
Summary: Eine Koenigin Unter den IB Monitoren

%define _bindir /opt/%{name}

%description
This package provides the ibtop command, along with the supporting
executables make-job-map and make-net-info.

%prep
%setup -q

%build
make VERSION=%{version} BINDIR=%{_bindir}

%install
rm -rf %{buildroot}
install -m 0755 -d %{buildroot}/%{_bindir}
install -m 0755 %{name} %{buildroot}/%{_bindir}/%{name}
install -m 0755 make-job-map %{buildroot}/%{_bindir}/make-job-map
install -m 0755 make-net-info %{buildroot}/%{_bindir}/make-net-info

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root,-)
%dir %{_bindir}/
%attr(0755,root,root) %{_bindir}/%{name}
%attr(0755,root,root) %{_bindir}/make-job-map
%attr(0755,root,root) %{_bindir}/make-net-info
