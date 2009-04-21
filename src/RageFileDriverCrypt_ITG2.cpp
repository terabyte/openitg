#include "global.h"
#include "RageLog.h"
#include "RageUtil.h"
#include "RageFile.h"
#include "RageException.h"
#include "RageFileDriverCrypt_ITG2.h"

// utilities for the crypto handling
#include "aes/aes.h"
#include "ibutton/ibutton.h"
#include "crypto/CryptSH512.h"

/* This is the additional key used for ITG2's patch files */
#define ITG2_PATCH_KEY "58691958710496814910943867304986071324198643072"

/* If no key is given, the driver will use the dongle for file keys.
 * Provide a default key for PC builds, and use the dongle for AC. */
#ifndef ITG_ARCADE
#define CRYPT_KEY "65487573252940086457044055343188392138734144585"
#else
#define CRYPT_KEY ""
#endif

/* Register all the file drivers we're going to be using */
REGISTER_ITG2_FILE_DRIVER( ITG2, "kry", CRYPT_KEY );
REGISTER_ITG2_FILE_DRIVER( PATCH, "patch", ITG2_PATCH_KEY );

// helpful little debug function
void print_hex( const CString &sName, const unsigned char *value, unsigned int length )
{
	CString sValues = "";

	for( unsigned i = 0; i < length; i++ )
		sValues += ssprintf( "%02X ", value[i] );

	LOG->Debug( "print_hex( %s ): %s", sName.c_str(), sValues.c_str() );
}

// contains pre-hashed decryption keys, for faster loading
// To do: move into RageFileDriverCrypt_ITG2?
typedef std::map<const char *, unsigned char*> tKeyMap;
tKeyMap g_KnownKeys;

RageFileDriverCrypt_ITG2::RageFileDriverCrypt_ITG2( const CString &sRoot, const CString &secret ): RageFileDriverCrypt(sRoot)
{
	m_sSecret = secret;
}

RageFileObjCrypt_ITG2::RageFileObjCrypt_ITG2( const RageFileObjCrypt_ITG2 &cpy ) : RageFileObjCrypt(cpy)
{
	// set the file size data
	m_iHeaderSize = cpy.m_iHeaderSize;
	m_iFileSize = cpy.m_iFileSize;

	// deep-copy the decryption ctx
	memcpy( m_ctx, cpy.m_ctx, sizeof(cpy.m_ctx) );
	m_sSecret = cpy.m_sSecret;
}

bool RageFileObjCrypt_ITG2::OpenInternal( const CString &sPath, int iMode, int &iError )
{
	// attempt to open the basic low-level routines for this file object
	if( !RageFileObjDirect::OpenInternal(sPath, iMode, iError) )
		return false;

	// attempt to read the header data
	char header[2];
	if( ReadDirect(&header, 2) < 2 )
	{
		LOG->Warn("RageFileObjCrypt_ITG2: Could not open %s: unexpected header size", sPath.c_str() );
		return false;
	}

	if( m_sSecret.empty() )
	{
		if (header[0] != ':' || header[1] != '|')
		{
			LOG->Warn("RageFileObjCrypt_ITG2: no secret given and %s is not an ITG2 arcade encrypted file", sPath.c_str() );
			return false;
		}
	}
	else
	{
		if (header[0] != '8' || header[1] != 'O')
		{
			LOG->Warn("RageFileObjCrypt_ITG2: secret given, but %s is not an ITG2 patch file", sPath.c_str() );
			return false;
		}
	}
	m_iHeaderSize = 2;

	if( ReadDirect(&m_iFileSize, 4) < 4 )
	{
		LOG->Warn("RageFileObjCrypt_ITG2: Could not open %s: unexpected file size", sPath.c_str() );
		return false;
	}
	m_iHeaderSize += 4;


	uint32_t subkey_size;
	if( ReadDirect(&subkey_size, 4) < 4 )
	{
		LOG->Warn("RageFileObjCrypt_ITG2: Could not open %s: unexpected subkey size", sPath.c_str() );
		return false;
	}
	m_iHeaderSize += 4;

	unsigned char *subkey = new unsigned char[subkey_size];

	uint32_t got = 0;
	if( (got = ReadDirect(subkey, subkey_size)) < subkey_size )
	{
		LOG->Warn("RageFileObjCrypt_ITG2: %s: subkey: expected %i, got %i", sPath.c_str(), subkey_size, got);
		return false;
	}
	m_iHeaderSize += subkey_size;

	unsigned char verifyblock[16];
	if ((got = ReadDirect(&verifyblock, 16)) < 16)
	{
		LOG->Warn("RageFileObjCrypt_ITG2: %s: verifyblock: expected 16, got %i", sPath.c_str(), got);
		return false;
	}
	m_iHeaderSize += 16;

	// try to find the key in our stored data, if possible - otherwise, generate it
	unsigned char *AESKey;
	tKeyMap::iterator iter = g_KnownKeys.find( sPath.c_str() );

	// no key found. generate it.
	if (iter == g_KnownKeys.end())
	{
		// allocate space for the AES key
		AESKey = new unsigned char[24];

		// no value was specified, so we need to grab one from the dongle
		if( m_sSecret.empty() )
		{
			CHECKPOINT_M( "dongle" );
			iButton::GetAESKey(subkey, AESKey);
		}
		else
		{
			// a value was specified, so we need to generate our own key
			CHECKPOINT_M( "patch" );
			unsigned char *SHABuffer = new unsigned char[subkey_size + 47];
			unsigned char *HashBuffer = new unsigned char[64];

			// copy the subkey into the SHA signing buffer
			memcpy(SHABuffer, subkey, subkey_size);

			// append the 47-byte secret key and sign the result
			memcpy(SHABuffer+subkey_size, m_sSecret.c_str(), 47);
			SHA512_Simple(SHABuffer, subkey_size+47, HashBuffer);

			// copy the first 24 bytes of the new hash to the AES key
			memcpy(AESKey, HashBuffer, 24);

			// de-allocate our now-unnecessary data
			SAFE_DELETE_ARRAY( SHABuffer );
			SAFE_DELETE_ARRAY( HashBuffer );
		}

		// save the key to the cache
		g_KnownKeys[sPath.c_str()] = AESKey;
	}
	else
	{
		// key found, so we can just save it here.
		CHECKPOINT_M("cache");
		AESKey = iter->second;
	}

	SAFE_DELETE_ARRAY( subkey );

	// decode the AES key into the decryption CTX
	aes_decrypt_key( AESKey, 24, m_ctx );

	// verify that this is the correct decryption key
	unsigned char plaintext[16];
	aes_decrypt( verifyblock, plaintext, m_ctx );

	if( plaintext[0] != ':' || plaintext[1] != 'D' )
	{
		LOG->Warn("RageFileObjCrypt_ITG2: %s: decrypt failed, unexpected decryption magic", sPath.c_str() );
		return false;
	}

	// everything's good. let's head on back.
	return true;
}

// when reading this, keep in mind that AES works in 16-byte blocks.
// startpos is rounded down to the nearest block we can decrypt from,
// endpos is rounded up to the farthest block we need to decrypt.
int RageFileObjCrypt_ITG2::ReadInternal( void *buffer, size_t bytes )
{
	unsigned int oldpos = Tell();

	// starting and ending decryption positions
	unsigned int startpos = (oldpos/16) * 16;
	unsigned int endpos = QuantizeUp(oldpos+bytes, 16);

	// amount of blocks to read
	size_t bufsize = endpos - startpos;
	ASSERT( bufsize % 16 == 0);

	// offset from decryption to relevant data
	unsigned int difference = oldpos - startpos;

	unsigned char *crbuf = new unsigned char[bufsize];
	unsigned char *dcbuf = new unsigned char[bufsize];

	ASSERT( crbuf );
	ASSERT( dcbuf );

	// initialize the back buffer and pre-XOR buffer
	unsigned char backbuffer[16], xorbuf[16];

	// initialize it with the 16 bytes prior to the start
	if( startpos % 4080 == 0 )
	{
		memset( backbuffer, '\0', 16 );
	}
	else
	{
		this->SeekInternal( m_iHeaderSize + (startpos-16) );
		this->ReadDirect( backbuffer, 16 );
	}

	// seek to the file location and read it into the buffer
	this->SeekInternal( m_iHeaderSize + startpos );
	this->ReadDirect( crbuf, bufsize );

	// TODO: understand this.
	for (unsigned i = 0; i < bufsize/16; i++)
	{
		// decrypt into the pre-XOR buffer
		aes_decrypt(crbuf+(i*16), xorbuf, m_ctx );

		// I don't understand this at all. :(
		for (unsigned char j = 0; j < 16; j++)
			dcbuf[(i*16)+j] = xorbuf[j] ^ (backbuffer[j] - j);

		if (((startpos + i*16) + 16) % 4080 != 0)
			memcpy(backbuffer, crbuf+(i*16), 16);
		else
			memset(backbuffer, '\0', 16);
	}

	memcpy(buffer, dcbuf+difference, bytes);

	this->SeekInternal( oldpos + bytes );

	delete[] crbuf;
	delete[] dcbuf;

	return bytes;
}

RageFileObjCrypt_ITG2 *RageFileObjCrypt_ITG2::Copy() const
{
        int iErr;
        RageFileObjCrypt_ITG2 *ret = new RageFileObjCrypt_ITG2(*this);

        if( ret->OpenInternal( m_sPath, m_iMode, iErr ) )
        {
                ret->SeekInternal( Tell() );
                return ret;
        }

        RageException::Throw( "Couldn't reopen \"%s\": %s", m_sPath.c_str(), strerror(iErr) );
        delete ret;
        return NULL;
}

/*
 * (c) 2009 BoXoRRoXoRs
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
