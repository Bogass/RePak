#include "pch.h"
#include "Assets.h"
#include <regex>

std::unordered_map<std::string, DataTableColumnDataType> DataTableColumnMap =
{
    { "bool",   DataTableColumnDataType::Bool },
    { "int",    DataTableColumnDataType::Int },
    { "float",  DataTableColumnDataType::Float },
    { "vector", DataTableColumnDataType::Vector },
    { "string", DataTableColumnDataType::StringT },
    { "asset",  DataTableColumnDataType::Asset },
    { "assetnoprecache", DataTableColumnDataType::AssetNoPrecache }
};

static std::regex s_VectorStringRegex("<(.*),(.*),(.*)>");

DataTableColumnDataType GetDataTableTypeFromString(std::string sType)
{
    std::transform(sType.begin(), sType.end(), sType.begin(), ::tolower);

    for (const auto& [key, value] : DataTableColumnMap) // Iterate through unordered_map.
    {
        if (sType.compare(key) == 0) // Do they equal?
            return value;
    }

    return DataTableColumnDataType::StringT;
}

uint32_t DataTable_GetEntrySize(DataTableColumnDataType type)
{
    switch (type)
    {
    case DataTableColumnDataType::Bool:
    case DataTableColumnDataType::Int:
    case DataTableColumnDataType::Float:
        return 4;
    case DataTableColumnDataType::Vector:
        return sizeof(float) * 3;
    case DataTableColumnDataType::StringT:
    case DataTableColumnDataType::Asset:
    case DataTableColumnDataType::AssetNoPrecache:
        // strings get placed at a separate place and have a pointer in place of the actual value
        return sizeof(RPakPtr);
    }

    return 0; // should be unreachable
}

void Assets::AddDataTableAsset(std::vector<RPakAssetEntryV8>* assetEntries, const char* assetPath, rapidjson::Value& mapEntry)
{
    Debug("Adding dtbl asset '%s'\n", assetPath);

    rapidcsv::Document doc(g_sAssetsDir + assetPath + ".csv");

    std::string sAssetName = assetPath;

    DataTableHeader* pHdr = new DataTableHeader();

    const size_t columnCount = doc.GetColumnCount();
    const size_t rowCount = doc.GetRowCount();

    if (columnCount < 0)
    {
        Warning("Attempted to add dtbl asset with no columns. Skipping asset...\n");
        return;
    }

    if (rowCount < 2)
    {
        Warning("Attempted to add dtbl asset with invalid row count. Skipping asset...\nDTBL    - CSV must have a row of column types at the end of the table\n");
        return;
    }

    size_t ColumnNameBufSize = 0;

    ///-------------------------------------
    // figure out the required name buf size
    for (auto& it : doc.GetColumnNames())
    {
        ColumnNameBufSize += it.length() + 1;
    }

    ///-----------------------------------------
    // make a page for the sub header
    //
    RPakVirtualSegment SubHeaderSegment{};
    _vseginfo_t subhdrinfo = RePak::CreateNewSegment(sizeof(DataTableHeader), 0, 8, SubHeaderSegment);

    // DataTableColumn entries
    RPakVirtualSegment ColumnHeaderSegment{};
    _vseginfo_t colhdrinfo = RePak::CreateNewSegment(sizeof(DataTableColumn) * columnCount, 1, 8, ColumnHeaderSegment, 64);

    // column names
    RPakVirtualSegment ColumnNamesSegment{};
    _vseginfo_t nameseginfo = RePak::CreateNewSegment(ColumnNameBufSize, 1, 8, ColumnNamesSegment, 64);


    pHdr->ColumnCount = columnCount;
    pHdr->RowCount = rowCount - 1;
    pHdr->ColumnHeaderPtr = { colhdrinfo.index, 0 };

    RePak::RegisterDescriptor(subhdrinfo.index, offsetof(DataTableHeader, ColumnHeaderPtr));

    // allocate buffers for the loop
    char* namebuf = new char[ColumnNameBufSize];
    char* columnHeaderBuf = new char[sizeof(DataTableColumn) * columnCount];

    // vectors
    std::vector<std::string> typeRow = doc.GetRow<std::string>(rowCount - 1);
    std::vector<DataTableColumn> columns{};

    uint32_t nextNameOffset = 0;
    uint32_t colIdx = 0;
    // temp var used for storing the row offset for the next column in the loop below
    uint32_t tempColumnRowOffset = 0;
    uint32_t stringEntriesSize = 0;
    size_t rowDataPageSize = 0;

    for (auto& it : doc.GetColumnNames())
    {
        // copy the column name into the namebuf
        snprintf(namebuf + nextNameOffset, it.length() + 1, "%s", it.c_str());

        DataTableColumnDataType type = GetDataTableTypeFromString(typeRow[colIdx]);

        DataTableColumn col{};

        // set the page index and offset
        col.NamePtr = { nameseginfo.index, nextNameOffset };
        col.RowOffset = tempColumnRowOffset;
        col.Type = type;

        columns.emplace_back(col);

        // register name pointer
        RePak::RegisterDescriptor(colhdrinfo.index, (sizeof(DataTableColumn) * colIdx) + offsetof(DataTableColumn, NamePtr));

        if (type == DataTableColumnDataType::StringT || type == DataTableColumnDataType::Asset || type == DataTableColumnDataType::AssetNoPrecache)
        {
            for (size_t i = 0; i < rowCount - 1; ++i)
            {
                // this can be std::string since we only really need to deal with the string types here
                std::vector<std::string> row = doc.GetRow<std::string>(i);

                stringEntriesSize += row[colIdx].length() + 1;
            }
        }

        *(DataTableColumn*)(columnHeaderBuf + (sizeof(DataTableColumn) * colIdx)) = col;

        tempColumnRowOffset += DataTable_GetEntrySize(type);
        rowDataPageSize += DataTable_GetEntrySize(type) * (rowCount - 1); // size of type * row count (excluding the type row)
        nextNameOffset += it.length() + 1;
        colIdx++;

        // if this is the final column, set the total row bytes to the column's row offset + the column's row size
        // (effectively the full length of the row)
        if (colIdx == columnCount)
            pHdr->RowStride = tempColumnRowOffset;
    }

    // page for Row Data
    RPakVirtualSegment RowDataSegment{};
    _vseginfo_t rawdatainfo = RePak::CreateNewSegment(rowDataPageSize, 1, 8, RowDataSegment, 64);

    // page for string entries
    RPakVirtualSegment StringEntrySegment{};
    _vseginfo_t stringsinfo = RePak::CreateNewSegment(stringEntriesSize, 1, 8, StringEntrySegment, 64);

    char* rowDataBuf = new char[rowDataPageSize];

    char* stringEntryBuf = new char[stringEntriesSize];

    for (size_t rowIdx = 0; rowIdx < rowCount - 1; ++rowIdx)
    {
        for (size_t colIdx = 0; colIdx < columnCount; ++colIdx)
        {
            DataTableColumn col = columns[colIdx];

            char* EntryPtr = (rowDataBuf + (pHdr->RowStride * rowIdx) + col.RowOffset);

            rmem valbuf(EntryPtr);

            switch (col.Type)
            {
            case DataTableColumnDataType::Bool:
            {
                std::string val = doc.GetCell<std::string>(colIdx, rowIdx);

                transform(val.begin(), val.end(), val.begin(), ::tolower);

                if (val == "true")
                    valbuf.write<uint32_t>(true);
                else
                    valbuf.write<uint32_t>(false);
                break;
            }
            case DataTableColumnDataType::Int:
            {
                uint32_t val = doc.GetCell<uint32_t>(colIdx, rowIdx);
                valbuf.write(val);
                break;
            }
            case DataTableColumnDataType::Float:
            {
                float val = doc.GetCell<float>(colIdx, rowIdx);
                valbuf.write(val);
                break;
            }
            case DataTableColumnDataType::Vector:
            {
                std::string val = doc.GetCell<std::string>(colIdx, rowIdx);

                std::smatch sm;

                // get values from format "<x,y,z>"
                std::regex_search(val, sm, s_VectorStringRegex);

                if (sm.size() == 4)
                {
                    float x = atof(sm[1].str().c_str());
                    float y = atof(sm[2].str().c_str());
                    float z = atof(sm[3].str().c_str());
                    Vector3 vec(x, y, z);

                    valbuf.write(vec);
                }
                break;
            }
            case DataTableColumnDataType::StringT:
            case DataTableColumnDataType::Asset:
            case DataTableColumnDataType::AssetNoPrecache:
            {
                static uint32_t nextStringEntryOffset = 0;

                RPakPtr stringPtr{ stringsinfo.index, nextStringEntryOffset };

                std::string val = doc.GetCell<std::string>(colIdx, rowIdx);
                snprintf(stringEntryBuf + nextStringEntryOffset, val.length() + 1, "%s", val.c_str());

                valbuf.write(stringPtr);
                RePak::RegisterDescriptor(rawdatainfo.index, (pHdr->RowStride * rowIdx) + col.RowOffset);

                nextStringEntryOffset += val.length() + 1;
                break;
            }
            }
        }
    }

    pHdr->RowHeaderPtr = { rawdatainfo.index, 0 };

    RePak::RegisterDescriptor(subhdrinfo.index, offsetof(DataTableHeader, RowHeaderPtr));

    // add raw data blocks
    RPakRawDataBlock shDataBlock{ subhdrinfo.index, subhdrinfo.size, (uint8_t*)pHdr };
    RePak::AddRawDataBlock(shDataBlock);

    RPakRawDataBlock colDataBlock{ colhdrinfo.index, colhdrinfo.size, (uint8_t*)columnHeaderBuf };
    RePak::AddRawDataBlock(colDataBlock);

    RPakRawDataBlock colNameDataBlock{ nameseginfo.index, nameseginfo.size, (uint8_t*)namebuf };
    RePak::AddRawDataBlock(colNameDataBlock);

    RPakRawDataBlock rowDataBlock{ rawdatainfo.index, rowDataPageSize, (uint8_t*)rowDataBuf };
    RePak::AddRawDataBlock(rowDataBlock);

    RPakRawDataBlock stringEntryDataBlock{ stringsinfo.index, stringEntriesSize, (uint8_t*)stringEntryBuf };
    RePak::AddRawDataBlock(stringEntryDataBlock);

    RPakAssetEntryV8 asset;

    asset.InitAsset(RTech::StringToGuid((sAssetName + ".rpak").c_str()), subhdrinfo.index, 0, subhdrinfo.size, rawdatainfo.index, 0, -1, -1, (std::uint32_t)AssetType::DTBL);
    asset.Version = DTBL_VERSION;

    asset.PageEnd = stringsinfo.index + 1; // number of the highest page that the asset references pageidx + 1
    asset.Un2 = 1;

    assetEntries->push_back(asset);
}