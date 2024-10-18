#include "Cheat.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include "esp_heap_caps.h" // Para heap_caps_malloc

// Variables estáticas
std::string CheatMngr::cheatFilename = "";
FILE* CheatMngr::cheatFileFP = nullptr;
Cheat* CheatMngr::cheats = nullptr;
Poke* CheatMngr::pokes = nullptr;
uint16_t CheatMngr::cheatCount = 0;
uint32_t CheatMngr::pokeCount = 0;

static char line[200]; // Buffer para leer líneas

// Función para liberar todos los datos
void CheatMngr::clearData() {
    if (cheats) heap_caps_free(cheats);
    if (pokes) heap_caps_free(pokes);
    cheats = nullptr;
    pokes = nullptr;
    cheatCount = 0;
    pokeCount = 0;
}

// Función para cerrar el archivo y liberar recursos
void CheatMngr::closeCheatFile() {
    clearData();
    if (cheatFileFP) {
        fclose(cheatFileFP);
        cheatFileFP = nullptr;
    }
}

// Función para contar POKEs y CHEATs
static void countCheatsAndPokes(FILE* file, uint16_t& cheatCount, uint32_t& pokeCount) {
    cheatCount = 0;
    pokeCount = 0;

    rewind(file); // Volver al inicio del archivo

    while (fgets(line, sizeof(line), file)) {
        if (line[0] == 'N') cheatCount++;
        else if (line[0] == 'M' || line[0] == 'Z') pokeCount++;
    }
}

// Función para cargar el archivo .pok
bool CheatMngr::loadCheatFile(const std::string& filename) {
    if (cheatFileFP) fclose(cheatFileFP);

    cheatFileFP = fopen(filename.c_str(), "rb");
    if (!cheatFileFP) {
        printf("Error: Could not open file %s\n", filename.c_str());
        return false;
    }

    cheatFilename = filename;
    clearData(); // Limpiar cualquier dato previo

    // 1. Contar cheats y pokes
    countCheatsAndPokes(cheatFileFP, cheatCount, pokeCount);

    // 2. Reservar memoria alineada para cheats y pokes
    cheats = (Cheat*)heap_caps_malloc(cheatCount * sizeof(Cheat), MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
    pokes = (Poke*)heap_caps_malloc(pokeCount * sizeof(Poke), MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);

    if (!cheats || !pokes) {
        printf("Error: Failed to allocate memory\n");
        closeCheatFile();
        return false;
    }

    // 3. Segunda pasada para cargar datos
    rewind(cheatFileFP);

    uint32_t pokeIdx = 0;
    uint16_t cheatIdx = 0;
    Cheat currentCheat = {};

    while (fgets(line, sizeof(line), cheatFileFP)) {

        if (line[0] == 'N') {
            if (currentCheat.pokeCount > 0) copyCheat(&currentCheat, &cheats[cheatIdx++]);
            currentCheat = {};
            currentCheat.nameOffset = ftell(cheatFileFP) - strlen(line);
            currentCheat.pokeStartIdx = pokeIdx;
        } else if (line[0] == 'M' || line[0] == 'Z') {
            Poke poke;
            uint16_t val;
            sscanf(line, "%*s %d %d %d %d", &poke.bank, &poke.address, &val, &poke.original);
            poke.is_input = (val == 256);
            poke.value = (poke.is_input ? poke.original : (uint8_t) val);
            copyPoke(&poke, &pokes[pokeIdx++]);
            if (poke.is_input) currentCheat.inputCount++;
            currentCheat.pokeCount++;
        }
    }

    copyCheat(&currentCheat, &cheats[cheatIdx++]);

    return true;
}

// Cheat

const Cheat CheatMngr::getCheat(int index) {
    if (index < 0 || index >= cheatCount) return {};
    Cheat c;
    copyCheat(&cheats[index], &c);
    return c;
}

const Cheat CheatMngr::toggleCheat(int index) {
    if (index < 0 || index >= cheatCount) return {};
    Cheat cheat;
    copyCheat(&cheats[index], &cheat);
    cheat.enabled = !cheat.enabled;
    copyCheat(&cheat, &cheats[index]);
    return cheat;
}

// Obtener el nombre del cheat
std::string CheatMngr::getCheatName(const Cheat& cheat) {
    if (!cheatFileFP) return "";
    fseek(cheatFileFP, cheat.nameOffset, SEEK_SET);
    if (fgets(line, sizeof(line), cheatFileFP)) {
        char* p = line;
        while (*p) {
            if (*p == '\r' || *p == '\n') *p = 0;
            ++p;
        }
    }
    return std::string(line + 1);
}

std::string CheatMngr::getCheatFilename() {
    return cheatFilename;
}

uint16_t CheatMngr::getCheatCount() {
    return cheatCount;
}

// Poke

const Poke CheatMngr::getPoke(const Cheat& cheat, size_t pokeIndex) {
    if (pokeIndex >= cheat.pokeCount) return {};
    Poke poke;
    copyPoke(&pokes[cheat.pokeStartIdx + pokeIndex], &poke);
    return poke;
}

const Poke CheatMngr::getInputPoke(const Cheat& cheat, size_t inputIndex) {
    size_t count = 0;
    for (int i = 0; i < cheat.pokeCount; ++i) {
        Poke poke;
        copyPoke(&pokes[cheat.pokeStartIdx + i], &poke);
        if (poke.is_input) {
            if (count == inputIndex) return poke;
            count++;
        }
    }
    return {};
}

const Poke CheatMngr::setPokeValue(const Cheat& cheat, size_t pokeIndex, uint8_t value) {
    if (pokeIndex >= cheat.pokeCount) return {};
    Poke poke;
    copyPoke(&pokes[cheat.pokeStartIdx + pokeIndex], &poke);
    poke.value = value;
    copyPoke(&poke, &pokes[cheat.pokeStartIdx + pokeIndex]);
    return poke;
}
