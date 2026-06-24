# RPM spec for cpclip (Fedora/RHEL). Build:
#   rpmbuild -ba packaging/cpclip.spec   (with the v%{version} tarball in SOURCES)
Name:           cpclip
Version:        0.1.0
Release:        1%{?dist}
Summary:        Minimal X11 and Wayland clipboard CLI

License:        GPLv2+
URL:            https://github.com/jvidal86/cpclip
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(xfixes)
BuildRequires:  pkgconfig(wayland-client)

%description
cpclip is a small command-line clipboard tool for Linux supporting both X11
(via Xfixes) and Wayland (via the ext-data-control protocol). A single binary
provides cpclip, cpadd, cppaste and cpclear via argv[0] dispatch. The Wayland
backend requires a compositor implementing ext_data_control_manager_v1.

%prep
%autosetup

%build
%set_build_flags
%make_build

%install
%make_install PREFIX=/usr

%files
%license LICENSE
%doc README.md
%{_bindir}/cpclip
%{_bindir}/cpadd
%{_bindir}/cppaste
%{_bindir}/cpclear
%{_mandir}/man1/cpclip.1*
%{_mandir}/man1/cpadd.1*
%{_mandir}/man1/cppaste.1*
%{_mandir}/man1/cpclear.1*

%changelog
* Wed Jun 24 2026 J Vidal <j.vidal.rodriguez@gmail.com> - 0.1.0-1
- Initial package.
