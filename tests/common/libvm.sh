# Source library for installed virtualized shell script tests
#
# Copyright (C) 2016 Jonathan Lebon <jlebon@redhat.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

# prepares the VM and library for action
vm_setup() {

  export VM=${VM:-vmcheck}
  local sshopts="-o User=root \
                 -o ControlMaster=auto \
                 -o ControlPath=${topsrcdir}/ssh-$VM.sock \
                 -o ControlPersist=yes"

  # If we're provided with an ssh-config, make sure we tell
  # ssh to pick it up.
  if [ -f "${topsrcdir}/ssh-config" ]; then
    sshopts="$sshopts -F ${topsrcdir}/ssh-config"
  fi

  export SSH="ssh $sshopts $VM"
  export SCP="scp $sshopts"
}

vm_rsync() {
  if ! test -f .vagrant/using_sshfs; then
    pushd ${topsrcdir}
    rsyncopts="ssh -o User=root"
    if [ -f ssh-config ]; then
      rsyncopts="$rsyncopts -F ssh-config"
    fi
    rsync -az --no-owner --no-group -e "$rsyncopts" \
              --exclude .git/ . $VM:/var/roothome/sync
    if test -n "${VMCHECK_INSTTREE:-}"; then
        rsync -az --no-owner --no-group -e "$rsyncopts" \
              ${VMCHECK_INSTTREE}/ $VM:/var/roothome/sync/insttree/
    fi
    popd
  fi
}

# run command in vm
# - $@    command to run
vm_cmd() {
  $SSH "$@"
}

# Copy argument (usually shell script) to VM, execute it there
vm_cmdfile() {
    bin=$1
    chmod a+x ${bin}
    bn=$(basename ${bin})
    $SCP $1 $VM:/root/${bn}
    $SSH /root/${bn}
}


# Delete anything which we might change between runs
vm_clean_caches() {
    vm_cmd rm /ostree/repo/extensions/rpmostree/pkgcache/refs/heads/* -rf
}

# run rpm-ostree in vm
# - $@    args
vm_rpmostree() {
    $SSH env ASAN_OPTIONS=detect_leaks=false rpm-ostree "$@"
}

# copy files to a directory in the vm
# - $1    target directory
# - $2..  files & dirs to copy
vm_send() {
  dir=$1; shift
  vm_cmd mkdir -p $dir
  $SCP -r "$@" $VM:$dir
}

# copy the test repo to the vm
vm_send_test_repo() {
  gpgcheck=${1:-0}
  vm_cmd rm -rf /tmp/vmcheck
  vm_send /tmp/vmcheck ${commondir}/compose/yum/repo

  cat > vmcheck.repo << EOF
[test-repo]
name=test-repo
baseurl=file:///tmp/vmcheck/repo
EOF

  if [ $gpgcheck -eq 1 ]; then
      cat >> vmcheck.repo <<EOF
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-fedora-25-primary
EOF
  else
      echo "Enabling vmcheck.repo without GPG"
      echo 'gpgcheck=0' >> vmcheck.repo
  fi

  vm_send /etc/yum.repos.d vmcheck.repo
}

# wait until ssh is available on the vm
# - $1    timeout in second (optional)
# - $2    previous bootid (optional)
vm_ssh_wait() {
  timeout=${1:-0}; shift
  old_bootid=${1:-}; shift
  if ! vm_cmd true; then
     echo "Failed to log into VM, retrying with debug:"
     $SSH -o LogLevel=debug true || true
  fi
  while [ $timeout -gt 0 ]; do
    if bootid=$(vm_get_boot_id 2>/dev/null); then
        if [[ $bootid != $old_bootid ]]; then
            # if this is a reboot, display some info about new boot
            if [ -n "$old_bootid" ]; then
              vm_rpmostree status
              vm_rpmostree --version
            fi
            return 0
        fi
    fi
    if test $(($timeout % 5)) == 0; then
        echo "Still failed to log into VM, retrying for $timeout seconds"
    fi
    timeout=$((timeout - 1))
    sleep 1
  done
  false "Timed out while waiting for SSH."
}

vm_get_boot_id() {
  vm_cmd cat /proc/sys/kernel/random/boot_id
}

# Run a command in the VM that will cause a reboot
vm_reboot_cmd() {
    vm_cmd sync
    bootid=$(vm_get_boot_id 2>/dev/null)
    vm_cmd $@ || :
    vm_ssh_wait 120 $bootid
}

# reboot the vm
vm_reboot() {
  vm_reboot_cmd systemctl reboot
}

# check that the given files/dirs exist on the VM
# - $@    files/dirs to check for
vm_has_files() {
  for file in "$@"; do
    if ! vm_cmd test -e $file; then
        return 1
    fi
  done
}

# check that the packages are installed
# - $@    packages to check for
vm_has_packages() {
  for pkg in "$@"; do
    if ! vm_cmd rpm -q $pkg; then
        return 1
    fi
  done
}

# retrieve info from a deployment
# - $1   index of deployment (or -1 for booted)
# - $2   key to retrieve
vm_get_deployment_info() {
  idx=$1
  key=$2
  vm_rpmostree status --json | \
    python -c "
import sys, json
deployments = json.load(sys.stdin)[\"deployments\"]
idx = $idx
if idx < 0:
  for i, depl in enumerate(deployments):
    if depl[\"booted\"]:
      idx = i
if idx < 0:
  print \"Failed to determine currently booted deployment\"
  exit(1)
if idx >= len(deployments):
  print \"Deployment index $idx is out of range\"
  exit(1)
depl = deployments[idx]
if \"$key\" in depl:
  data = depl[\"$key\"]
  if type(data) is list:
    print \" \".join(data)
  else:
    print data
"
}

# retrieve info from the booted deployment
# - $1   key to retrieve
vm_get_booted_deployment_info() {
  vm_get_deployment_info -1 $1
}

# print the layered packages
vm_get_layered_packages() {
  vm_get_booted_deployment_info packages
}

# print the requested packages
vm_get_requested_packages() {
  vm_get_booted_deployment_info requested-packages
}

vm_get_local_packages() {
  vm_get_booted_deployment_info requested-local-packages
}

# check that the packages are currently layered
# - $@    packages to check for
vm_has_layered_packages() {
  pkgs=$(vm_get_layered_packages)
  for pkg in "$@"; do
    if [[ " $pkgs " != *$pkg* ]]; then
        return 1
    fi
  done
}

# check that the packages are currently requested
# - $@    packages to check for
vm_has_requested_packages() {
  pkgs=$(vm_get_requested_packages)
  for pkg in "$@"; do
    if [[ " $pkgs " != *$pkg* ]]; then
        return 1
    fi
  done
}

vm_has_local_packages() {
  pkgs=$(vm_get_local_packages)
  for pkg in "$@"; do
    if [[ " $pkgs " != *$pkg* ]]; then
        return 1
    fi
  done
}

vm_has_dormant_packages() {
  vm_has_requested_packages "$@" && \
    ! vm_has_layered_packages "$@"
}

# retrieve the checksum of the currently booted deployment
vm_get_booted_csum() {
  vm_get_booted_deployment_info checksum
}

# make multiple consistency checks on a test pkg
# - $1    package to check for
# - $2    either "present" or "absent"
vm_assert_layered_pkg() {
  pkg=$1; shift
  policy=$1; shift

  set +e
  vm_has_packages $pkg;         pkg_in_rpmdb=$?
  vm_has_layered_packages $pkg; pkg_is_layered=$?
  vm_has_local_packages $pkg;   pkg_is_layered_local=$?
  vm_has_requested_packages $pkg; pkg_is_requested=$?
  [ $pkg_in_rpmdb == 0 ] && \
  ( ( [ $pkg_is_layered == 0 ] &&
      [ $pkg_is_requested == 0 ] ) ||
    [ $pkg_is_layered_local == 0 ] ); pkg_present=$?
  [ $pkg_in_rpmdb != 0 ] && \
  [ $pkg_is_layered != 0 ] && \
  [ $pkg_is_layered_local != 0 ] && \
  [ $pkg_is_requested != 0 ]; pkg_absent=$?
  set -e

  if [ $policy == present ] && [ $pkg_present != 0 ]; then
    vm_cmd rpm-ostree status
    assert_not_reached "pkg $pkg is not present"
  fi

  if [ $policy == absent ] && [ $pkg_absent != 0 ]; then
    vm_cmd rpm-ostree status
    assert_not_reached "pkg $pkg is not absent"
  fi
}

vm_assert_status_jq() {
    vm_rpmostree status --json > status.json
    assert_status_file_jq status.json "$@"
}
