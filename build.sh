#!/bin/bash

# --- CONFIGURAÇÃO ---
# Defina o caminho para a pasta principal do seu Clang.
LLVM_DIR="$HOME/DalheKernel/ClangDA"
# Defina a pasta onde os arquivos finais serão salvos.
OUTPUT_DIR="Releases"

# --- INÍCIO DO SCRIPT ---

# Menu de seleção para o tipo de compilação
echo "Escolha o tipo de pacote de saída:"
echo "  1) AnyKernel3 (.zip para Recovery)"
echo "  2) Fastboot (boot.img para flashear via Fastboot)"
read -p "Digite 1 ou 2: " BUILD_CHOICE

# Define o alvo da compilação com base na escolha
case $BUILD_CHOICE in
    1)
        BUILD_TARGET="Image.gz dtbs"
        PACKAGE_TYPE="AnyKernel3"
        ;;
    2)
        BUILD_TARGET="bootimg"
        PACKAGE_TYPE="Fastboot"
        ;;
    *)
        echo "❌ Opção inválida. Saindo."
        exit 1
        ;;
esac

echo "✅ Tipo de pacote selecionado: $PACKAGE_TYPE"

# Pede um nome para o Kernel
read -p "Digite um nome para o arquivo do kernel (ex: PowerKernel-v1): " KERNEL_NAME
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

echo "🚀 Iniciando a compilação para $PACKAGE_TYPE..."

# Comando de compilação principal.
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
    LLVM_IAS=1 $BUILD_TARGET

# --- EMPACOTAMENTO E VERIFICAÇÃO ---

# Cria o diretório de saída se ele não existir
mkdir -p $OUTPUT_DIR

# Empacota com base na escolha do usuário
if [ "$PACKAGE_TYPE" == "AnyKernel3" ]; then
    # Verifica se a compilação do AnyKernel deu certo
    IMAGE="out/arch/arm64/boot/Image.gz"
    if [ ! -f "$IMAGE" ]; then
        echo "❌ ERRO: A compilação falhou! O arquivo Image.gz não foi criado."
        exit 1
    fi
    echo "✅ Compilação concluída com sucesso!"
    echo "📦 Empacotando para AnyKernel3..."

    DTB_DIR="out/arch/arm64/boot/dts/vendor"
    cp $IMAGE AnyKernel3/Image.gz
    cat ${DTB_DIR}/*.dtb > AnyKernel3/dtb

    # Cria o zip do AnyKernel3
    cd AnyKernel3
    rm -f *.zip
    zip -r9 "${KERNEL_NAME}.zip" .
    # Move o zip para a pasta de saída
    mv "${KERNEL_NAME}.zip" ../$OUTPUT_DIR/
    cd ..
    FINAL_FILE="${OUTPUT_DIR}/${KERNEL_NAME}.zip"

elif [ "$PACKAGE_TYPE" == "Fastboot" ]; then
    # Verifica se a compilação do boot.img deu certo
    BOOT_IMG="out/arch/arm64/boot/boot.img"
    if [ ! -f "$BOOT_IMG" ]; then
        echo "❌ ERRO: A compilação falhou! O arquivo boot.img não foi criado."
        exit 1
    fi
    echo "✅ Compilação concluída com sucesso!"
    echo "📦 Movendo boot.img..."
    # Move o boot.img para a pasta de saída, renomeando-o
    mv $BOOT_IMG "${OUTPUT_DIR}/${KERNEL_NAME}-boot.img"
    FINAL_FILE="${OUTPUT_DIR}/${KERNEL_NAME}-boot.img"
fi

# Finaliza o cronômetro e mostra o tempo
DATE_END=$(date +"%s")
DIFF=$(($DATE_END - $DATE_START))

echo "---------------------------------------------------"
echo "🎉 Tudo pronto! O seu kernel está em: ${FINAL_FILE}"
echo "⏰ Tempo total de compilação: $(($DIFF / 60)) minuto(s) e $(($DIFF % 60)) segundos."
echo "---------------------------------------------------"
