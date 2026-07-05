
# vagrant box add 
Vagrant.configure("2") do |config|
  config.vm.define "my-ubuntu-vm" do |vm|

    vm.vm.box = "perk/ubuntu-25.04-arm64"
    vm.vm.box_version = "20250424" 
    
    vm.vm.box_architecture = "arm64"    
    
    vm.vm.provider "qemu" do |qemu|
      qemu.ssh_port = "50022"
      qemu.memory = "4096"
      qemu.cpus = 2
    end
     
    vm.vm.synced_folder ".", "/home/vagrant/uefi-boot-lab",
      type: "rsync",
      rsync__args: ["--verbose", "--archive", "--delete", "-z", "--exclude", "artifacts/"],
      rsync__exclude: ["node_modules/", "artifacts/", ".DS_Store"], rsync__auto: true

    vm.vm.provision "shell", inline: <<-SHELL
        # Stop immediately on failures and print each command while provisioning.
        set -eux

        # Refresh package metadata before installing development tools.
        sudo apt-get update  
        # sudo apt-get upgrade -y


        # General command-line tools used while working inside the VM.
        sudo apt-get install -y \
          ca-certificates apt-transport-https software-properties-common \
          build-essential git curl wget vim htop unzip plocate gnupg lsb-release

        # UEFI/EDK II lab dependencies:
        # - acpica-tools provides iasl for ACPI-related builds.
        # - nasm is required by EDK II BaseTools and many X64 firmware builds.
        # - gcc/binutils x86_64 cross tools let this ARM64 VM build X64 EFI apps.
        # - qemu-system-x86 and ovmf are used to run the built EFI app.
        # - dosfstools/mtools are useful for creating or inspecting FAT boot media.
        sudo apt-get install -y \
          acpica-tools nasm uuid-dev python3 python3-pip \
          gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu \
          qemu-system-x86 ovmf dosfstools mtools

        # Keep EDK II outside the synced project folder so generated build output
        # and submodules do not pollute this repository.
        sudo -u vagrant mkdir -p /home/vagrant/workspace
        if [ ! -d /home/vagrant/workspace/edk2/.git ]; then
          sudo -u vagrant git clone https://github.com/tianocore/edk2.git /home/vagrant/workspace/edk2
        fi

        # Initialize EDK II submodules and compile BaseTools once during provisioning.
        cd /home/vagrant/workspace/edk2
        sudo -u vagrant git submodule update --init
        sudo -u vagrant make -C BaseTools

        # EDK II expects this repo to appear as UefiBootLabPkg under the workspace.
        sudo -u vagrant ln -sfn /home/vagrant/uefi-boot-lab /home/vagrant/workspace/edk2/UefiBootLabPkg

        # These exports make scripts/build-firmware-view.sh work after `vagrant ssh`.
        # Modern EDK II uses GCC/GCCNOLTO toolchain tags and GCC_BIN as the
        # compiler prefix. GCC5 was removed from current upstream tools_def.txt.
        grep -qxF 'export EDK2_WORKSPACE=$HOME/workspace/edk2' /home/vagrant/.bashrc || echo 'export EDK2_WORKSPACE=$HOME/workspace/edk2' >> /home/vagrant/.bashrc
        grep -qxF 'export EDK2_TOOLCHAIN=GCC' /home/vagrant/.bashrc || echo 'export EDK2_TOOLCHAIN=GCC' >> /home/vagrant/.bashrc
        grep -qxF 'export GCC_BIN=x86_64-linux-gnu-' /home/vagrant/.bashrc || echo 'export GCC_BIN=x86_64-linux-gnu-' >> /home/vagrant/.bashrc
        grep -qxF 'export GCCNOLTO_BIN=x86_64-linux-gnu-' /home/vagrant/.bashrc || echo 'export GCCNOLTO_BIN=x86_64-linux-gnu-' >> /home/vagrant/.bashrc

        # Put later downloads in /tmp instead of the EDK II checkout.
        cd /tmp
        
        # Extra kernel, tracing, and build tooling. These are not required for the
        # current FirmwareView app, but can be useful for later Linux boot experiments.
        sudo apt-get install -y \
          zlib1g-dev libelf-dev libzstd-dev pkg-config \
          clang llvm linux-headers-$(uname -r) \
          linux-tools-$(uname -r) linux-tools-common \
          bsdutils virtme-ng systemtap-sdt-dev \
          libseccomp-dev protobuf-compiler \
          python3 python3-pip ninja-build meson cmake
      
        # Optional Go toolchain for general development inside this VM.
        GO_VERSION="1.24.2"
        wget -q https://go.dev/dl/go${GO_VERSION}.linux-arm64.tar.gz
        sudo rm -rf /usr/local/go
        sudo tar -C /usr/local -xzf go${GO_VERSION}.linux-arm64.tar.gz
        rm go${GO_VERSION}.linux-arm64.tar.gz
        # Persist Go environment variables for interactive shells.
        echo 'export PATH=$PATH:/usr/local/go/bin' >> /home/vagrant/.bashrc
        echo 'export GOPATH=$HOME/go' >> /home/vagrant/.bashrc
        echo 'export PATH=$PATH:$GOPATH/bin' >> /home/vagrant/.bashrc            

        # Optional terminal UI for Kubernetes clusters.
        curl -LO https://github.com/derailed/k9s/releases/download/v0.50.15/k9s_Linux_arm64.tar.gz
        tar -xzf k9s_Linux_arm64.tar.gz
        sudo mv ./k9s /usr/local/bin/
        sudo chmod +x /usr/local/bin/k9s

        # Optional Docker installation for later container-based experiments.
        sudo mkdir -p /etc/apt/keyrings
        curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
        echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
        sudo apt-get update
        sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
        sudo usermod -aG docker vagrant      

        # Optional Kubernetes CLI tools for non-UEFI development work.
        curl -fsSL https://pkgs.k8s.io/core:/stable:/v1.29/deb/Release.key  | sudo gpg --dearmor -o /etc/apt/keyrings/kubernetes-apt-keyring.gpg
        echo "deb [signed-by=/etc/apt/keyrings/kubernetes-apt-keyring.gpg] https://pkgs.k8s.io/core:/stable:/v1.29/deb/ /" | sudo tee /etc/apt/sources.list.d/kubernetes.list
        sudo apt-get update
        sudo apt-get install -y kubectl
        
        curl https://raw.githubusercontent.com/helm/helm/main/scripts/get-helm-3 | bash
    SHELL
  end  
end
