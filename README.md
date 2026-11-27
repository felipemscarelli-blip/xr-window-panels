<img src="assets/icon-256.png" alt="XR Window Panels" width="96" align="right">

# XR Window Panels

Camada (API layer) OpenXR que mostra janelas do Windows como painéis dentro de apps e jogos VR em Direct3D 11 (iRacing, ACC, LMU, AMS2, DesktopXR...). Feita para levar overlays de telemetria e coaching para dentro do VR, como as linhas de acelerador e freio de uma volta de referência.

> **English:** A lightweight OpenXR API layer that shows any desktop window (e.g. telemetry or coaching overlays) as a floating panel inside D3D11 OpenXR games. It includes a control app to pick windows and position them live. The capture runs off the game's render thread for minimal performance impact. Download the zip from [Releases](../../releases), extract it and run `Instalar.cmd`.

## Instalar

1. Baixe o zip mais recente em [Releases](../../releases) e extraia numa pasta.
2. Dê dois cliques em **`Instalar.cmd`** pelo Explorador de Arquivos e aceite o aviso de administrador.

- A camada é registrada para a máquina toda (HKLM).
- O app de controle é copiado e ganha um atalho na área de trabalho: **XR Window Panels**.

Rode o instalador pelo Explorador ou por um PowerShell normal, nunca de dentro de um app empacotado (MSIX). O Windows desvia as gravações desses apps para uma cópia privada, e os jogos não enxergam a camada.

Para remover, use **`Desinstalar.cmd`**. O `config.ini` é mantido.

## App de controle (`XRWindowPanelsControl.exe`)

- **Painéis**: adicione um painel por janela.
- **Janela**: escolha a janela na lista. Ela é identificada por "Título contém" e "Processo". Para janelas cujo título muda (como as abas do Chrome), apague o título e deixe só o processo.
- **Posição**: estes botões podem ser segurados para repetir.
  - Recentralizar (este painel) e Recentralizar todos
  - Mover ▲◀▶▼
  - Maior/Menor
  - Mais perto/Mais longe
  - Inclinar/girar
  - O "Passo" escolhe entre ajuste fino, normal e grosso.
- **Aparência**: opacidade, remover cor de fundo (chroma key), transparência da própria janela.
- **Atualizações/s**: quantas vezes por segundo a imagem do painel é atualizada. Quanto menor, mais leve.
- O rodapé mostra se há um jogo VR conectado, e o status de cada painel aparece no VR.

As mudanças aparecem no VR na hora. Ficam salvas em `%LOCALAPPDATA%\XRWindowPanels\config.ini`.

- **Lembrar posição recentralizada** (opção no grupo Posição, desligada por padrão): com ela ligada, ao recentralizar um painel, a camada salva essa posição em `anchors.ini` e a restaura na próxima vez que você abrir o jogo. Desligada, a recentralização vale só enquanto o jogo estiver aberto.
  - A posição fica no espaço STAGE do runtime, que é fixo na sala. Assim o painel volta ao mesmo lugar do cockpit mesmo que o runtime recentralize a visão.
  - Se o runtime não tiver STAGE, a camada usa o espaço LOCAL.
  - Se você refizer a configuração da sala no headset, recentralize de novo.
- **Iniciar com o Windows**: marque a opção no rodapé do app. Ele passa a abrir junto com o Windows, já minimizado na bandeja, para os atalhos funcionarem desde o começo.
  - Abrir o app de novo (pelo atalho) traz a janela que já está aberta, sem abrir uma segunda cópia.

## Atalhos no jogo

Atalhos fixos da camada (funcionam mesmo sem o app aberto):

- `Ctrl+F12`: mostra/esconde os painéis. Escondidos, eles não gastam nada de GPU.
- `Ctrl+F11`: recentraliza todos os painéis na frente da cabeça.

### Atalhos personalizados: teclado, volante e button box

No app de controle, clique em **⌨ Atalhos...** e dê um duplo clique numa ação. Depois aperte a tecla (pode combinar com Ctrl, Shift ou Alt) ou o botão do volante, da button box ou do joystick.

- **Ações disponíveis**:
  - Recentralizar (selecionado / todos)
  - Mostrar/esconder todos
  - Próximo/anterior painel
  - Ativar/desativar painel
  - Mover ▲◀▶▼, Maior/Menor, Mais perto/longe, Inclinar/girar. Esses movimentos repetem enquanto o botão estiver pressionado.
- **Qual painel se move**: as ações de movimento valem para o painel selecionado. Ao trocar de painel, ele ganha uma **borda amarela por 1,5 s no VR**, para você saber qual está selecionado.
- **Controles**: volantes, button boxes e joysticks são lidos via Raw Input (HID), sem limite de botões (bases Simagic, por exemplo, expõem mais de 100), incluindo o direcional (POV/hat). A janela de captura mostra ao vivo o que cada controle está enviando.
- **App aberto**: os atalhos funcionam com o jogo em foco, desde que o app esteja aberto. Minimizado, ele vai para a bandeja e continua funcionando.
- **Onde ficam salvos**: `%LOCALAPPDATA%\XRWindowPanels\bindings.ini`.

## Desempenho

O objetivo é que a camada quase não custe nada para o jogo:

- **Fora da thread de renderização do jogo**: buscar a janela, iniciar a captura e ler a configuração rodam em threads separadas, de baixa prioridade.
- **Captura separada do jogo**: usa um dispositivo gráfico próprio (com prioridade de GPU reduzida) e passa a imagem por uma textura compartilhada. Nada espera: se o recurso estiver ocupado, tenta de novo no frame seguinte.
- **Trabalho só quando muda**: a GPU só trabalha quando a janela muda, e no máximo nas atualizações/s configuradas (30 por padrão). Nos outros frames, o custo é só repetir o painel.
- **Estado do jogo intocado**: o estado do Direct3D do jogo é salvo e restaurado só nos frames em que há algo para desenhar.

## Log

`%LOCALAPPDATA%\XRWindowPanels\layer.log`

## Compilar

Precisa do Visual Studio 2022 Build Tools (C++), do CMake e do git. O app de controle usa o `csc.exe` do .NET Framework, que já vem no Windows.

```powershell
cmake -S . -B <pasta-curta>\build -G "Visual Studio 17 2022" -A x64
cmake --build <pasta-curta>\build --config Release
```

Use uma pasta de build com caminho curto: o MSBuild falha com caminhos acima de 260 caracteres.

Ferramenta de teste sem VR:

```powershell
capture_test --list
capture_test --config
capture_test "Google Chrome" saida.bmp
```

## Limitações

- Funciona só com jogos em Direct3D 11.
- Não dá para interagir com o painel usando os controles de VR.
