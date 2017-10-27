/*
 * ======================================================================
 * Copyright (c) Microsoft Open Technologies, Inc.  All rights reserved.
 * Licensed under the MIT License.
 * See LICENSE.md in the project root for license information.
 * ======================================================================
*/
#include "PDFProtector.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#include "BlockBasedProtectedStream.h"
#include "ModernAPI/RMSExceptions.h"
#include "Common/CommonTypes.h"
#include "Core/ProtectionPolicy.h"
#include "Platform/Logger/Logger.h"

namespace logger = rmscore::platform::logger;

namespace rmscore {
namespace fileapi {

////////////////////////////////////////////////////////////////////////////
/// class PDFProtector
PDFCryptoHandlerImpl::PDFCryptoHandlerImpl(std::shared_ptr<PDFProtector> pPDFProtector)
{
    m_pPDFProtector = pPDFProtector;
    m_bProgressiveStart = false;

    m_dataToDecrypted = nullptr;
    m_objnum = 0;

    m_sharedProtectedStream = nullptr;
    m_outputIOS = nullptr;
    m_outputSharedStream = nullptr;
}

PDFCryptoHandlerImpl::~PDFCryptoHandlerImpl()
{
    m_pPDFProtector = nullptr;
}

uint32_t PDFCryptoHandlerImpl::DecryptGetSize(uint32_t src_size)
{
    return src_size;
}

void PDFCryptoHandlerImpl::DecryptStart(uint32_t objnum, uint32_t gennum)
{
    m_objnum = objnum;
    m_dataToDecrypted = std::make_shared<std::stringstream>();
}

bool PDFCryptoHandlerImpl::DecryptStream(char* src_buf, uint32_t src_size, pdfobjectmodel::PDFBinaryBuf* dest_buf)
{
    m_dataToDecrypted->write(src_buf, src_size);
    return true;
}

bool PDFCryptoHandlerImpl::DecryptFinish(pdfobjectmodel::PDFBinaryBuf* dest_buf)
{
    m_dataToDecrypted->seekg(0, std::ios::end);
    uint64_t count = m_dataToDecrypted->tellg();
    if(count <= 0)
    {
        m_objnum = 0;
        m_dataToDecrypted.reset();
        m_dataToDecrypted = nullptr;
        return true;
    }

    std::shared_ptr<std::iostream> protectedIOS = m_dataToDecrypted;
    auto inputSharedStream = rmscrypto::api::CreateStreamFromStdStream(protectedIOS);
    auto protectedStream = m_pPDFProtector->CreateProtectedStream(inputSharedStream, count);

    std::shared_ptr<std::stringstream> outputSS = std::make_shared<std::stringstream>();
    std::shared_ptr<std::iostream> outputIOS = outputSS;
    auto outputSharedStream = rmscrypto::api::CreateStreamFromStdStream(protectedIOS);

    m_pPDFProtector->DecryptStream(outputSharedStream, protectedStream, count);

    outputSharedStream->Seek(std::ios::beg);
    auto nSize = outputSharedStream->Size();
    std::vector<uint8_t> decryptedData = outputSharedStream->Read(nSize);

    char bufDestSize[4];
    memset(bufDestSize, 0, 4 * sizeof(char));
    bufDestSize[0] = decryptedData[3];
    bufDestSize[1] = decryptedData[2];
    bufDestSize[2] = decryptedData[1];
    bufDestSize[3] = decryptedData[0];
    uint64_t dwTotalDestSize = 0;
    memcpy(&dwTotalDestSize, bufDestSize, 4);

    dest_buf->AppendBlock(reinterpret_cast<const char*>(&decryptedData[0] + 4), dwTotalDestSize);

    m_objnum = 0;
    m_dataToDecrypted.reset();
    m_dataToDecrypted = nullptr;

    return true;
}

uint32_t PDFCryptoHandlerImpl::EncryptGetSize(uint32_t objnum, uint32_t version, char* src_buf, uint32_t src_size)
{
    uint32_t encryptedSize = 0;
    encryptedSize = src_size;

    encryptedSize += 4; //add prefix padding
    encryptedSize += (16 - encryptedSize % 16);//add postfix padding

    return encryptedSize;
}

bool PDFCryptoHandlerImpl::EncryptContent(uint32_t objnum, uint32_t version, char* src_buf, uint32_t src_size, char* dest_buf, uint32_t* dest_size)
{
    if(!m_pPDFProtector) return false;

    uint64_t contentSizeAddPre = src_size + 4;
    std::shared_ptr<char> sharedContentAddPre(new char[contentSizeAddPre]);
    char* contentAddPre = sharedContentAddPre.get();
    contentAddPre[3] = ((char*)&src_size)[0];
    contentAddPre[2] = ((char*)&src_size)[1];
    contentAddPre[1] = ((char*)&src_size)[2];
    contentAddPre[0] = ((char*)&src_size)[3];
    memcpy(contentAddPre + 4, src_buf, src_size);

    auto sstream = std::make_shared<std::stringstream>();
    std::shared_ptr<std::iostream> outputIOS = sstream;

    auto outputSharedStream = rmscrypto::api::CreateStreamFromStdStream(outputIOS);
    auto protectedStream = m_pPDFProtector->CreateProtectedStream(outputSharedStream, contentSizeAddPre);
    m_pPDFProtector->EncryptStream(contentAddPre, contentSizeAddPre, protectedStream, true);

    outputSharedStream->Seek(std::ios::beg);
    auto nSize = outputSharedStream->Size();
    int64_t dataRead = outputSharedStream->Read(reinterpret_cast<uint8_t*>(dest_buf), nSize);
    *dest_size = nSize;

    return true;
}

bool PDFCryptoHandlerImpl::ProgressiveEncryptStart(uint32_t objnum, uint32_t version, uint32_t raw_size)
{
    if (raw_size > MIN_RAW_SIZE)
    {
        m_bProgressiveStart = true;
        return true;
    }
    return false;
}

bool PDFCryptoHandlerImpl::ProgressiveEncryptContent(uint32_t objnum, uint32_t version, char* src_buf, uint32_t src_size, pdfobjectmodel::PDFBinaryBuf* dest_buf)
{
    uint64_t contentSizeAddPre = 0;
    char* contentAddPre = nullptr;

    if(m_bProgressiveStart)
    {
        contentSizeAddPre = src_size + 4;
        std::shared_ptr<char> sharedContentAddPre(new char[contentSizeAddPre]);
        contentAddPre = sharedContentAddPre.get();

        contentAddPre[3] = ((char*)&src_size)[0];
        contentAddPre[2] = ((char*)&src_size)[1];
        contentAddPre[1] = ((char*)&src_size)[2];
        contentAddPre[0] = ((char*)&src_size)[3];
        memcpy(contentAddPre + 4, src_buf, src_size);

        auto sstream = std::make_shared<std::stringstream>();
        m_outputIOS = sstream;

        m_outputSharedStream = rmscrypto::api::CreateStreamFromStdStream(m_outputIOS);
        m_sharedProtectedStream = m_pPDFProtector->CreateProtectedStream(m_outputSharedStream, contentSizeAddPre);
    }
    else
    {
        contentSizeAddPre = src_size;
        contentAddPre = src_buf;
    }

    m_pPDFProtector->EncryptStream(contentAddPre, contentSizeAddPre, m_sharedProtectedStream, false);

    m_bProgressiveStart = false;

    return true;
}

bool PDFCryptoHandlerImpl::ProgressiveEncryptFinish(pdfobjectmodel::PDFBinaryBuf* dest_buf)
{
    m_pPDFProtector->EncryptStream(nullptr, 0, m_sharedProtectedStream, true);

    m_outputSharedStream->Seek(std::ios::beg);
    auto nSize = m_outputSharedStream->Size();

    std::shared_ptr<char> sharedDataRead(new char[nSize]);
    char* pDataRead = sharedDataRead.get();
    int64_t dataRead = m_outputSharedStream->Read(reinterpret_cast<uint8_t*>(pDataRead), nSize);

    dest_buf->AppendBlock(pDataRead, nSize);

    m_sharedProtectedStream.reset();
    m_outputIOS.reset();
    m_outputSharedStream.reset();

    m_sharedProtectedStream = nullptr;
    m_outputIOS = nullptr;
    m_outputSharedStream = nullptr;

    return true;
}

////////////////////////////////////////////////////////////////////////////
/// class PDFSecurityHandlerImpl

PDFSecurityHandlerImpl::PDFSecurityHandlerImpl(std::shared_ptr<PDFProtector> pPDFProtector,
        const UserContext &userContext,
        const UnprotectOptions &options,
        std::shared_ptr<std::atomic<bool> > cancelState)
    : m_userContext(userContext),
      m_options(options),
      m_cancelState(cancelState)
{
    m_pPDFProtector = pPDFProtector;
    m_pCryptoHandler = nullptr;
}

PDFSecurityHandlerImpl::~PDFSecurityHandlerImpl()
{
    m_pCryptoHandler.reset();
    m_pCryptoHandler = nullptr;
}

bool PDFSecurityHandlerImpl::OnInit(unsigned char *publishingLicense, uint32_t plSize)
{
    std::vector<uint8_t> vecPL(plSize);
    memcpy(reinterpret_cast<uint8_t *>(&vecPL[0]), publishingLicense, plSize);
    //vecPL.push_back('\0');

    //testing code, save publishing license
   /*std::string plPath = "C:\\Users\\foxit-dev\\Desktop\\PL.txt";
   auto outPLFile = std::make_shared<std::fstream>(
     plPath, std::ios_base::in | std::ios_base::out | std::ios_base::trunc | std::ios_base::binary);
   outPLFile->write(reinterpret_cast<const char*>(publishingLicense), plSize);
   outPLFile->close();
   */

    modernapi::PolicyAcquisitionOptions policyAcquisitionOptions = m_options.offlineOnly?
                modernapi::PolicyAcquisitionOptions::POL_OfflineOnly :
                modernapi::PolicyAcquisitionOptions::POL_None;
    auto cacheMask = modernapi::RESPONSE_CACHE_NOCACHE;
    if (m_options.useCache)
    {
        cacheMask = static_cast<modernapi::ResponseCacheFlags>(modernapi::RESPONSE_CACHE_INMEMORY|
                                                               modernapi::RESPONSE_CACHE_ONDISK |
                                                               modernapi::RESPONSE_CACHE_CRYPTED);
    }

    auto policyRequest = modernapi::UserPolicy::Acquire(vecPL,
                                                        m_userContext.userId,
                                                        m_userContext.authenticationCallback,
                                                        &m_userContext.consentCallback,
                                                        policyAcquisitionOptions,
                                                        cacheMask,
                                                        m_cancelState);
    if (policyRequest->Status != modernapi::GetUserPolicyResultStatus::Success)
    {
        logger::Logger::Error("UserPolicy::Acquire unsuccessful", policyRequest->Status);
        throw exceptions::RMSPDFFileException("The file may be corrupt or the user may have no righs.",
                                            exceptions::RMSPDFFileException::CannotAcquirePolicy);
        return false;
    }

    std::shared_ptr<modernapi::UserPolicy> userPolicy = policyRequest->Policy;
    if (userPolicy.get() == nullptr)
    {
        logger::Logger::Error("User Policy acquisition failed");
        throw exceptions::RMSInvalidArgumentException("User Policy acquisition failed.");
        return false;
    }

    bool bIsIssuedToOwner = userPolicy->IsIssuedToOwner();
    if(!bIsIssuedToOwner)
    {
        logger::Logger::Error("Only the owner has the right to unprotect the document.");
        throw exceptions::RMSException(exceptions::RMSException::ExceptionTypes::LogicError,
                                       exceptions::RMSException::ErrorTypes::RightsError,
                                       "Only the owner has the right to unprotect the document.");
    }

    m_pPDFProtector->SetUserPolicy(userPolicy);

    return true;
}

std::shared_ptr<pdfobjectmodel::PDFCryptoHandler> PDFSecurityHandlerImpl::CreateCryptoHandler()
{
    m_pCryptoHandler = std::make_shared<PDFCryptoHandlerImpl>(m_pPDFProtector);
    return m_pCryptoHandler;
}

////////////////////////////////////////////////////////////////////////////
/// class PDFProtector

PDFProtector::PDFProtector(const std::string& originalFilePath,
                           const std::string& originalFileExtension,
                           std::shared_ptr<std::fstream> inputStream)
    : m_originalFileExtension(originalFileExtension),
      m_inputStream(inputStream),
      m_originalFilePath(originalFilePath)
{
    pdfobjectmodel::PDFModuleMgr::Initialize();
    m_pdfCreator = pdfobjectmodel::PDFCreator::Create();
    m_inputWrapperStream = nullptr;
}

PDFProtector::~PDFProtector()
{
}

void PDFProtector::SetWrapper(std::shared_ptr<std::fstream> inputWrapperStream)
{
    m_inputWrapperStream = inputWrapperStream;
}

void PDFProtector::ProtectWithTemplate(const UserContext& userContext,
                                         const ProtectWithTemplateOptions& options,
                                         std::shared_ptr<std::fstream> outputStream,
                                         std::shared_ptr<std::atomic<bool>> cancelState)
{
    logger::Logger::Hidden("+PDFProtector::ProtectWithTemplate");
    if (!outputStream->is_open())
    {
        logger::Logger::Error("Output stream invalid");
        throw exceptions::RMSStreamException("Output stream invalid");
    }

    if (IsProtected())
    {
        logger::Logger::Error("File is already protected");
        throw exceptions::RMSPDFFileException("File is already protected",
                                            exceptions::RMSPDFFileException::AlreadyProtected);
        return;
    }

    auto userPolicyCreationOptions = ConvertToUserPolicyCreationOptions(
                options.allowAuditedExtraction, options.cryptoOptions);
    m_userPolicy = modernapi::UserPolicy::CreateFromTemplateDescriptor(options.templateDescriptor,
                                                                       userContext.userId,
                                                                       userContext.authenticationCallback,
                                                                       userPolicyCreationOptions,
                                                                       options.signedAppData,
                                                                       cancelState);
    Protect(outputStream);
    logger::Logger::Hidden("-PDFProtector::ProtectWithTemplate");
}

void PDFProtector::ProtectWithCustomRights(const UserContext& userContext,
                                             const ProtectWithCustomRightsOptions& options,
                                             std::shared_ptr<std::fstream> outputStream,
                                             std::shared_ptr<std::atomic<bool>> cancelState)
{
    logger::Logger::Hidden("+PDFProtector::ProtectWithCustomRights");
    if (!outputStream->is_open())
    {
        logger::Logger::Error("Output stream invalid");
        throw exceptions::RMSStreamException("Output stream invalid");
    }

    if (IsProtected())
    {
        logger::Logger::Error("File is already protected");
        throw exceptions::RMSPDFFileException("File is already protected",
                                            exceptions::RMSPDFFileException::AlreadyProtected);
    }

    auto userPolicyCreationOptions = ConvertToUserPolicyCreationOptions(
                options.allowAuditedExtraction, options.cryptoOptions);
    m_userPolicy = modernapi::UserPolicy::Create(
                const_cast<modernapi::PolicyDescriptor&>(options.policyDescriptor),
                userContext.userId,
                userContext.authenticationCallback,
                userPolicyCreationOptions,
                cancelState);
    Protect(outputStream);
    logger::Logger::Hidden("-PDFProtector::ProtectWithCustomRights");
}

UnprotectResult PDFProtector::Unprotect(const UserContext& userContext,
                                          const UnprotectOptions& options,
                                          std::shared_ptr<std::fstream> outputStream,
                                          std::shared_ptr<std::atomic<bool>> cancelState)
{
    logger::Logger::Hidden("+PDFProtector::UnProtect");
    if (!outputStream->is_open())
    {
        logger::Logger::Error("Output stream invalid");
        throw exceptions::RMSStreamException("Output stream invalid");
        return rmscore::fileapi::UnprotectResult::FAILURE;
    }

    std::shared_ptr<std::iostream> inputEncryptedIO = m_inputStream;
    auto inputEncrypted = rmscrypto::api::CreateStreamFromStdStream(inputEncryptedIO);

    std::unique_ptr<pdfobjectmodel::PDFWrapperDoc> pdfWrapperDoc =  pdfobjectmodel::PDFWrapperDoc::Create(inputEncrypted);
    uint32_t wrapperType = pdfWrapperDoc->GetWrapperType();
    uint32_t payloadSize = pdfWrapperDoc->GetPayLoadSize();
    std::wstring wsGraphicFilter;
    float fVersion = 0;
    pdfWrapperDoc->GetCryptographicFilter(wsGraphicFilter, fVersion);
    if((wrapperType != PDFWRAPPERDOC_TYPE_IRMV1 && wrapperType != PDFWRAPPERDOC_TYPE_IRMV2)
            || (payloadSize <= 0)
            || wsGraphicFilter.compare(PDF_PROTECTOR_WRAPPER_SUBTYPE) != 0)
    {
        logger::Logger::Error("It is not a valid RMS-protected file.");
        throw exceptions::RMSPDFFileException("It is not a valid RMS-protected file.",
                                            exceptions::RMSPDFFileException::NotValidFile);
        return rmscore::fileapi::UnprotectResult::FAILURE;
    }

    auto payloadSS = std::make_shared<std::stringstream>();
    std::shared_ptr<std::iostream> payloadIOS = payloadSS;
    auto outputPayload = rmscrypto::api::CreateStreamFromStdStream(payloadIOS);
    bool bGetPayload = pdfWrapperDoc->StartGetPayload(outputPayload);

    std::shared_ptr<std::iostream> outputDecryptedIO = outputStream;
    auto outputDecrypted = rmscrypto::api::CreateStreamFromStdStream(outputDecryptedIO);

    std::string filterName = PDF_PROTECTOR_FILTER_NAME;

    std::shared_ptr<PDFProtector> sharedPDFProtector(this, [=](PDFProtector* pPDFProtector)
    {
        pPDFProtector = nullptr;
    });

    auto securityHander = std::make_shared<PDFSecurityHandlerImpl>(sharedPDFProtector, userContext, options, cancelState);
    uint32_t result = m_pdfCreator->UnprotectCustomEncryptedFile(
                outputPayload,
                filterName,
                securityHander,
                outputDecrypted);
    if(PDFCREATOR_ERR_SUCCESS != result)
    {
        logger::Logger::Error("Failed to decrypt the file. The file may be corrupted.");
        throw exceptions::RMSPDFFileException("Failed to decrypt the file. The file may be corrupted.",
                                            exceptions::RMSPDFFileException::CorruptFile);
        return rmscore::fileapi::UnprotectResult::FAILURE;
    }

    logger::Logger::Hidden("+PDFProtector::UnProtect");
    return rmscore::fileapi::UnprotectResult::SUCCESS;
}

bool PDFProtector::IsProtected() const
{
    logger::Logger::Hidden("+PDFProtector::IsProtected");

    std::shared_ptr<std::iostream> inputEncryptedIO = m_inputStream;
    auto inputEncrypted = rmscrypto::api::CreateStreamFromStdStream(inputEncryptedIO);

    std::unique_ptr<pdfobjectmodel::PDFWrapperDoc> pdfWrapperDoc =  pdfobjectmodel::PDFWrapperDoc::Create(inputEncrypted);
    uint32_t wrapperType = pdfWrapperDoc->GetWrapperType();
    uint32_t payloadSize = pdfWrapperDoc->GetPayLoadSize();
    std::wstring wsGraphicFilter;
    float fVersion = 0;
    pdfWrapperDoc->GetCryptographicFilter(wsGraphicFilter, fVersion);
    if((wrapperType != PDFWRAPPERDOC_TYPE_IRMV1 && wrapperType != PDFWRAPPERDOC_TYPE_IRMV2)
            || (payloadSize <= 0)
            || wsGraphicFilter.compare(PDF_PROTECTOR_WRAPPER_SUBTYPE) != 0)
    {
        return false;
    }

    logger::Logger::Hidden("The document is protected with rms.");
    logger::Logger::Hidden("-PDFProtector::IsProtected");
    return true;
}

void PDFProtector::Protect(const std::shared_ptr<std::fstream>& outputStream)
{
    if (m_userPolicy.get() == nullptr)
    {
        logger::Logger::Error("User Policy creation failed");
        throw exceptions::RMSInvalidArgumentException("User Policy creation failed.");
    }

    auto publishingLicense = m_userPolicy->SerializedPolicy();

    auto encryptedSS = std::make_shared<std::stringstream>();
    std::shared_ptr<std::iostream> encryptedIOS = encryptedSS;
    auto outputEncrypted = rmscrypto::api::CreateStreamFromStdStream(encryptedIOS);

    std::string filterName = PDF_PROTECTOR_FILTER_NAME;

    std::shared_ptr<PDFProtector> sharedPDFProtector(this, [=](PDFProtector* pPDFProtector)
    {
        pPDFProtector = nullptr;
    });

    auto cryptoHander = std::make_shared<PDFCryptoHandlerImpl>(sharedPDFProtector);
    uint32_t result = m_pdfCreator->CreateCustomEncryptedFile(
                m_originalFilePath,
                filterName,
                publishingLicense,
                cryptoHander,
                outputEncrypted);
    if(PDFCREATOR_ERR_SUCCESS != result)
    {
        logger::Logger::Error("Failed to encrypt the file. The file is invalid.");
        throw exceptions::RMSPDFFileException("Failed to encrypt the file. The file is invalid.",
                                            exceptions::RMSPDFFileException::CorruptFile);
        return;
    }

    if(!m_inputWrapperStream)
    {
        logger::Logger::Error("Not set the input wrapper stream.");
        throw exceptions::RMSInvalidArgumentException("Not set the input wrapper stream.");
        return;
    }
    std::shared_ptr<std::iostream> inputWrapperIO = m_inputWrapperStream;
    auto inputWrapper = rmscrypto::api::CreateStreamFromStdStream(inputWrapperIO);
    m_pdfWrapperCreator = pdfobjectmodel::PDFUnencryptedWrapperCreator::Create(inputWrapper);
    m_pdfWrapperCreator->SetPayloadInfo(
                PDF_PROTECTOR_WRAPPER_SUBTYPE,
                PDF_PROTECTOR_WRAPPER_FILENAME,
                PDF_PROTECTOR_WRAPPER_DES,
                PDF_PROTECTOR_WRAPPER_VERSION);
    m_pdfWrapperCreator->SetPayLoad(outputEncrypted);

    std::shared_ptr<std::iostream> outputIO = outputStream;
    auto outputWrapper = rmscrypto::api::CreateStreamFromStdStream(outputIO);
    bool bResult = m_pdfWrapperCreator->CreateUnencryptedWrapper(outputWrapper);
    if(!bResult)
    {
        logger::Logger::Error("Failed to create PDF IRM V2 file. The wrapper doc may be invalid.");
        throw exceptions::RMSInvalidArgumentException("Failed to create PDF IRM V2 file. The wrapper doc may be invalid.");
        return;
    }
}

std::shared_ptr<rmscrypto::api::BlockBasedProtectedStream> PDFProtector::CreateProtectedStream(
        const rmscrypto::api::SharedStream& stream,
        uint64_t contentSize)
{
    auto protectionPolicy = m_userPolicy->GetImpl();
    /*if (protectionPolicy->GetCipherMode() == rmscrypto::api::CipherMode::CIPHER_MODE_ECB)
    {
        // Older versions of the SDK ignored ECB cipher mode when encrypting pfile format.
        protectionPolicy->ReinitilizeCryptoProvider(rmscrypto::api::CipherMode::CIPHER_MODE_CBC4K);
    }*/

    auto cryptoProvider = m_userPolicy->GetImpl()->GetCryptoProvider();
    m_blockSize = cryptoProvider->GetBlockSize();
    // Cache block size to be 512 for cbc512, 4096 for cbc4k and ecb
    uint64_t protectedStreamBlockSize = m_blockSize == 512 ? 512 : 4096;
    auto backingStreamImpl = stream->Clone();
    uint64_t contentStartPosition = 0;
    return rmscrypto::api::BlockBasedProtectedStream::Create(cryptoProvider,
                                                             backingStreamImpl,
                                                             contentStartPosition,
                                                             contentSize,
                                                             protectedStreamBlockSize);
}

void PDFProtector::EncryptStream(
        char* pBuffer,
        uint32_t bufferSize,
        const std::shared_ptr<rmscrypto::api::BlockBasedProtectedStream>& pStream,
        bool bFinish)
{
    if(nullptr == pBuffer || 0 == bufferSize)
    {
        if(bFinish)
        {
            pStream->Flush();
        }
        return;
    }

    uint32_t contentSize = bufferSize;

    uint64_t bufSize = 4096;    //should be a multiple of AES block size (16)
    std::vector<uint8_t> buffer(bufSize);
    uint64_t readPosition  = 0;
    uint64_t writePosition = 0;
    bool isECB = m_userPolicy->DoesUseDeprecatedAlgorithms();
    uint64_t totalSize = isECB? ((contentSize + m_blockSize - 1) & ~(m_blockSize - 1)) :
                                contentSize;

    while(totalSize - readPosition > 0)
    {
        uint64_t offsetRead  = readPosition;
        uint64_t offsetWrite = writePosition;
        uint64_t toProcess   = std::min(bufSize, totalSize - readPosition);
        readPosition  += toProcess;
        writePosition += toProcess;

       memcpy(reinterpret_cast<char *>(&buffer[0]), pBuffer + offsetRead, toProcess);

        auto written = pStream->WriteAsync(
                    buffer.data(), toProcess, offsetWrite, std::launch::deferred).get();

        if (written != toProcess)
        {
          throw exceptions::RMSStreamException("Error while writing data");
        }
    }

    if(bFinish)
    {
        pStream->Flush();
    }
}

void PDFProtector::DecryptStream(const rmscrypto::api::SharedStream& outputIOS,
        const std::shared_ptr<rmscrypto::api::BlockBasedProtectedStream>& pStream,
        uint64_t originalContentSize)
{
    uint64_t bufSize = 4096;    //should be a multiple of AES block size (16)
    std::vector<uint8_t> buffer(bufSize);
    uint64_t readPosition  = 0;
    uint64_t writePosition = 0;
    uint64_t totalSize = pStream->Size();
    while(totalSize - readPosition > 0)
    {
        uint64_t offsetRead  = readPosition;
        uint64_t offsetWrite = writePosition;
        uint64_t toProcess   = std::min(bufSize, totalSize - readPosition);
        uint64_t originalRemaining = std::min(bufSize, originalContentSize - readPosition);
        readPosition  += toProcess;
        writePosition += toProcess;

        auto read = pStream->ReadAsync(
                    &buffer[0], toProcess, offsetRead, std::launch::deferred).get();
        if (read == 0)
        {
          break;
        }

        outputIOS->Seek(offsetWrite);
        outputIOS->Write(buffer.data(), originalRemaining);
    }
    outputIOS->Flush();
}

modernapi::UserPolicyCreationOptions PDFProtector::ConvertToUserPolicyCreationOptions(
        const bool& allowAuditedExtraction,
        CryptoOptions cryptoOptions)
{
    auto userPolicyCreationOptions = allowAuditedExtraction ?
                modernapi::UserPolicyCreationOptions::USER_AllowAuditedExtraction :
                modernapi::UserPolicyCreationOptions::USER_None;
    if (cryptoOptions == CryptoOptions::AES128_ECB )
    {
        userPolicyCreationOptions = static_cast<modernapi::UserPolicyCreationOptions>(
                    userPolicyCreationOptions |
                    modernapi::UserPolicyCreationOptions::USER_PreferDeprecatedAlgorithms);
    }
    return userPolicyCreationOptions;
}

void PDFProtector::SetUserPolicy(std::shared_ptr<modernapi::UserPolicy> userPolicy)
{
    m_userPolicy = userPolicy;
}

} // namespace fileapi
} // namespace rmscore