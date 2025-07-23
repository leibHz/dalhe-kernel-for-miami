#!/bin/bash

# --- CONFIGURAÇÃO ---
# Defina o caminho para a pasta principal do seu Clang.
# O script vai encontrar os binários dentro dela automaticamente.
LLVM_DIR="$HOME/DalheKernel/ClangDA"

# --- INÍCIO DO SCRIPT ---
# Pede um nome para o Kernel
read -p "Digite um nome para o kernel: " KERNEL_NAME
if [ -z "$KERNEL_NAME" ]; then
    echo "❌ Nome do kernel não pode ser vazio. Saindo."
    exit 1
fi

# Checa se a pasta do Clang existe
if [ ! -d "$LLVM_DIR" ]; then
    echo "❌ A pasta do Clang não foi encontrada em: $LLVM_DIR"
    echo "❌ Verifique a variável LLVM_DIR no início do script."
    exit 1
fi

echo "✅ Usando Clang de: $LLVM_DIR"
echo "✅ Limpando compilações antigas..."

# Limpa a compilação antiga (mrproper)
make O=out ARCH=arm64 mrproper

# Configura o kernel com os defconfigs
echo "✅ Configurando o kernel..."
make O=out ARCH=arm64 vendor/holi-qgki_defconfig vendor/debugfs.config vendor/ext_config/lineage_moto-holi.config

# Inicia o cronômetro
DATE_START=$(date +"%s")

echo "🚀 Iniciando a compilação do Kernel e DTBs..."

# Comando de compilação principal.
# Passamos o caminho completo para cada ferramenta para evitar erros de ambiente.
make -j$(nproc --all) \
    O=out \
    ARCH=arm64 \
    CC="$LLVM_DIR/bin/clang" \
    LD="$LLVM_DIR/bin/ld.lld" \
    AR="$LLVM_DIR/bin/llvm-ar" \
    NM="$LLVM_DIR/bin/llvm-nm" \
    OBJCOPY="$LLVM_DIR/bin/llvm-objcopy" \
    OBJDUMP="$LLVM_DIR/bin/llvm-objdump" \
    STRIP="$LLVM_DIR/bin/llvm-strip" \
    HOSTCC="$LLVM_DIR/bin/clang" \
    HOSTLD="$LLVM_DIR/bin/ld.lld" \
    CROSS_COMPILE="aarch64-linux-gnu-" \
    LLVM=1 \
    LLVM_IAS=1 Image.gz dtbs

# --- EMPACOTAMENTO E VERIFICAÇÃO ---

# Define os caminhos dos artefatos de saída
IMAGE="out/arch/arm64/boot/Image.gz"
DTB_DIR="out/arch/arm64/boot/dts/vendor"

# VERIFICA SE A COMPILAÇÃO DEU CERTO ANTES DE CONTINUAR
if [ ! -f "$IMAGE" ]; then
    echo "❌ ERRO: A compilação falhou! O arquivo Image.gz não foi criado."
    exit 1
fi

echo "✅ Compilação concluída com sucesso!"
echo "📦 Empacotando para o AnyKernel3..."

# Copia a Imagem e combina os DTBs
cp $IMAGE AnyKernel3/Image.gz
cat ${DTB_DIR}/*.dtb > AnyKernel3/dtb

# Cria o zip do AnyKernel3
cd AnyKernel3
rm -f *.zip
zip -r9 "${KERNEL_NAME}.zip" .
cd ..

# Finaliza o cronômetro e mostra o tempo
DATE_END=$(date +"%s")
DIFF=$(($DATE_END - $DATE_START))

echo "---------------------------------------------------"
echo "🎉 Tudo pronto! O seu kernel é: AnyKernel3/${KERNEL_NAME}.zip"
echo "⏰ Tempo total de compilação: $(($DIFF / 60)) minuto(s) e $(($DIFF % 60)) segundos."
echo "---------------------------------------------------"