// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: debsrcrecords.cc,v 1.6 2004/03/17 05:58:54 mdz Exp $
/* ######################################################################
   
   Debian Source Package Records - Parser implementation for Debian style
                                   source indexes
      
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <config.h>

#include <apt-pkg/deblistparser.h>
#include <apt-pkg/debsrcrecords.h>
#include <apt-pkg/error.h>
#include <apt-pkg/strutl.h>
#include <apt-pkg/aptconfiguration.h>
#include <apt-pkg/srcrecords.h>
#include <apt-pkg/tagfile.h>
#include <apt-pkg/hashes.h>
#include <apt-pkg/gpgv.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>
									/*}}}*/

using std::max;
using std::string;

// SrcRecordParser::Binaries - Return the binaries field		/*{{{*/
// ---------------------------------------------------------------------
/* This member parses the binaries field into a pair of class arrays and
   returns a list of strings representing all of the components of the
   binaries field. The returned array need not be freed and will be
   reused by the next Binaries function call. This function is commonly
   used during scanning to find the right package */
const char **debSrcRecordParser::Binaries()
{
   const char *Start, *End;
   if (Sect.Find("Binary", Start, End) == false)
      return NULL;
   for (; isspace(*Start) != 0; ++Start);
   if (Start >= End)
      return NULL;

   StaticBinList.clear();
   free(Buffer);
   Buffer = strndup(Start, End - Start);

   char* bin = Buffer;
   do {
      char* binStartNext = strchrnul(bin, ',');
      char* binEnd = binStartNext - 1;
      for (; isspace(*binEnd) != 0; --binEnd)
	 binEnd = '\0';
      StaticBinList.push_back(bin);
      if (*binStartNext != ',')
	 break;
      *binStartNext = '\0';
      for (bin = binStartNext + 1; isspace(*bin) != 0; ++bin);
   } while (*bin != '\0');
   StaticBinList.push_back(NULL);

   return &StaticBinList[0];
}
									/*}}}*/
// SrcRecordParser::BuildDepends - Return the Build-Depends information	/*{{{*/
// ---------------------------------------------------------------------
/* This member parses the build-depends information and returns a list of 
   package/version records representing the build dependency. The returned 
   array need not be freed and will be reused by the next call to this 
   function */
bool debSrcRecordParser::BuildDepends(std::vector<pkgSrcRecords::Parser::BuildDepRec> &BuildDeps,
					bool const &ArchOnly, bool const &StripMultiArch)
{
   unsigned int I;
   const char *Start, *Stop;
   BuildDepRec rec;
   const char *fields[] = {"Build-Depends", 
                           "Build-Depends-Indep",
			   "Build-Conflicts",
			   "Build-Conflicts-Indep"};

   BuildDeps.clear();

   for (I = 0; I < 4; I++) 
   {
      if (ArchOnly && (I == 1 || I == 3))
         continue;

      if (Sect.Find(fields[I], Start, Stop) == false)
         continue;
      
      while (1)
      {
         Start = debListParser::ParseDepends(Start, Stop, 
		     rec.Package,rec.Version,rec.Op,true,StripMultiArch,true);
	 
         if (Start == 0) 
            return _error->Error("Problem parsing dependency: %s", fields[I]);
	 rec.Type = I;

	 if (rec.Package != "")
   	    BuildDeps.push_back(rec);
	 
   	 if (Start == Stop) 
	    break;
      }	 
   }
   
   return true;
}
									/*}}}*/
// SrcRecordParser::Files - Return a list of files for this source	/*{{{*/
// ---------------------------------------------------------------------
/* This parses the list of files and returns it, each file is required to have
   a complete source package */
bool debSrcRecordParser::Files(std::vector<pkgSrcRecords::File> &List)
{
   List.erase(List.begin(),List.end());

   // map from the Hashsum field to the hashsum function,
   // unfortunately this is not a 1:1 mapping from
   // Hashes::SupporedHashes as e.g. Files is a historic name for the md5
   const std::pair<const char*, const char*> SourceHashFields[] = {
      std::make_pair( "Checksums-Sha512",  "SHA512"),
      std::make_pair( "Checksums-Sha256",  "SHA256"),
      std::make_pair( "Checksums-Sha1",  "SHA1"),
      std::make_pair( "Files",  "MD5Sum"),      // historic Name
   };
   
   for (unsigned int i=0;
        i < sizeof(SourceHashFields)/sizeof(SourceHashFields[0]);
        i++)
   {
      string Files = Sect.FindS(SourceHashFields[i].first);
      if (Files.empty() == true)
         continue;

      // Stash the / terminated directory prefix
      string Base = Sect.FindS("Directory");
      if (Base.empty() == false && Base[Base.length()-1] != '/')
         Base += '/';

      std::vector<std::string> const compExts = APT::Configuration::getCompressorExtensions();

      // Iterate over the entire list grabbing each triplet
      const char *C = Files.c_str();
      while (*C != 0)
      {   
         pkgSrcRecords::File F;
         string Size;
         
         // Parse each of the elements
         std::string RawHash;
         if (ParseQuoteWord(C, RawHash) == false ||
             ParseQuoteWord(C, Size) == false ||
             ParseQuoteWord(C, F.Path) == false)
            return _error->Error("Error parsing '%s' record", 
                                 SourceHashFields[i].first);
         // assign full hash string
         F.Hash = HashString(SourceHashFields[i].second, RawHash).toStr();
         // API compat hack 
         if(strcmp(SourceHashFields[i].second, "MD5Sum") == 0)
            F.MD5Hash = RawHash;
         
         // Parse the size and append the directory
         F.Size = atoi(Size.c_str());
         F.Path = Base + F.Path;
         
         // Try to guess what sort of file it is we are getting.
         string::size_type Pos = F.Path.length()-1;
         while (1)
         {
            string::size_type Tmp = F.Path.rfind('.',Pos);
            if (Tmp == string::npos)
               break;
            if (F.Type == "tar") {
               // source v3 has extension 'debian.tar.*' instead of 'diff.*'
               if (string(F.Path, Tmp+1, Pos-Tmp) == "debian")
                  F.Type = "diff";
               break;
            }
            F.Type = string(F.Path,Tmp+1,Pos-Tmp);
            
            if (std::find(compExts.begin(), compExts.end(), std::string(".").append(F.Type)) != compExts.end() ||
                F.Type == "tar")
            {
               Pos = Tmp-1;
               continue;
            }
	 
            break;
         }
      
         List.push_back(F);
      }
      break;
   }
   return (List.size() > 0);
}
									/*}}}*/
// SrcRecordParser::~SrcRecordParser - Destructor			/*{{{*/
// ---------------------------------------------------------------------
/* */
debSrcRecordParser::~debSrcRecordParser()
{
   delete[] Buffer;
}
									/*}}}*/


debDscRecordParser::debDscRecordParser(std::string const &DscFile, pkgIndexFile const *Index)
   : debSrcRecordParser(DscFile, Index)
{
   // support clear signed files
   if (OpenMaybeClearSignedFile(DscFile, Fd) == false)
   {
      _error->Error("Failed to open %s", DscFile.c_str());
      return;
   }

   // re-init to ensure the updated Fd is used
   Tags.Init(&Fd);
   // read the first (and only) record
   Step();

}
