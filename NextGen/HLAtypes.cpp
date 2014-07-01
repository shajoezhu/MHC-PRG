/*
 * HLAtypes.cpp
 *
 *  Created on: 20.06.2014
 *      Author: AlexanderDilthey
 */

#include "HLAtypes.h"


#include <iostream>
#include <fstream>
#include <exception>
#include <stdexcept>
#include <assert.h>
#include <vector>
#include <utility>
#include <algorithm>
#include <omp.h>
#include <functional>
#include <cctype>
#include <set>

#include "readSimulator.h"
#include "../Utilities.h"
#include <boost/math/distributions/poisson.hpp>

#include "../hash/deBruijn/DeBruijnGraph.h"
#include "../GraphAligner/GraphAlignerAffine.h"
#include "../GraphAlignerUnique/GraphAlignerUnique.h"

#include "Validation.h"

#include <stdio.h>
#include "dirent.h"


// constants
double log_likelihood_insertion = log(0.00001);
double log_likelihood_deletion = log_likelihood_insertion;
// int max_mismatches_perRead = 2;
double min_alignmentFraction_OK = 0.96; // measures all alignment positions but graph AND sequence gaps, separately for both reads
double min_oneRead_weightedCharactersOK = 0.995; // one read, mismatches downweighted by quality
double min_bothReads_weightedCharactersOK = 0.985; // both reads, mismatches downweighted by quality


class oneExonPosition {
public:
	unsigned int positionInExon;
	int graphLevel;
	std::string genotype;
	std::string alignment_edgelabels;
	std::string qualities;

	std::string thisRead_ID;
	double thisRead_fractionOK;
	double thisRead_WeightedCharactersOK;

	std::string pairedRead_ID;
	double pairedRead_fractionOK;
	double pairedRead_WeightedCharactersOK;

	bool pairs_strands_OK;
	double pairs_strands_distance;
};


auto countMismatchesInExon = [](std::vector<oneExonPosition>& exonPositions) -> int {
	int forReturn = 0;
	for(unsigned int i = 0; i < exonPositions.size(); i++)
	{
		if(exonPositions.at(i).genotype != exonPositions.at(i).alignment_edgelabels)
		{
			forReturn++;
		}
	}
	return forReturn;
};


auto alignmentFractionOK = [](seedAndExtend_return_local& r) -> double {
	int positions_OK = 0;
	int positions_checked = 0;

	for(unsigned int i = 0; i < r.graph_aligned.size(); i++)
	{
		if((r.graph_aligned.at(i) == '_') && (r.sequence_aligned.at(i) == '_'))
		{
			continue;
		}

		positions_checked++;
		if(r.graph_aligned.at(i) == r.sequence_aligned.at(i))
		{
			positions_OK++;
		}
	}
	return double(positions_OK)/double(positions_checked);
};

auto printPerc = [](double v1, double v2) -> std::string {
	double perc = (v1/v2) * 100;
	return Utilities::DtoStr(perc);
};

auto meanMedian = [](std::vector<double> L) -> std::pair<double, double> {
	std::sort(L.begin(), L.end());
	double S = 0;
	for(unsigned int i = 0; i < L.size(); i++)
	{
		S += L.at(i);
		// std::cout << L.at(i) << " ";
	}
	double mean = 0;
	double median = 0;
	if(L.size() > 0)
	{
		mean = S / (double)L.size();
		unsigned int medium_index = L.size() / 2;
		median = L.at(medium_index);
	}
	return make_pair(mean, median);
};


auto alignmentWeightedOKFraction = [&](oneRead& underlyingRead, seedAndExtend_return_local& alignment) -> double {
	int indexIntoOriginalReadData = -1;

	int totalMismatches = 0;
	double weightedMismatches = 0;

	for(unsigned int cI = 0; cI < alignment.sequence_aligned.length(); cI++)
	{
		std::string sequenceCharacter = alignment.sequence_aligned.substr(cI, 1);
		std::string graphCharacter = alignment.graph_aligned.substr(cI, 1);
		int graphLevel = alignment.graph_aligned_levels.at(cI);

		if(sequenceCharacter != "_")
		{
			indexIntoOriginalReadData++;
			int indexIntoOriginalReadData_correctlyAligned = indexIntoOriginalReadData;
			if(alignment.reverse)
			{
				indexIntoOriginalReadData_correctlyAligned = underlyingRead.sequence.length() - indexIntoOriginalReadData_correctlyAligned - 1;
			}
			assert(indexIntoOriginalReadData_correctlyAligned >= 0);
			assert(indexIntoOriginalReadData_correctlyAligned < (int)underlyingRead.sequence.length());;

			std::string underlyingReadCharacter = underlyingRead.sequence.substr(indexIntoOriginalReadData_correctlyAligned, 1);
			if(alignment.reverse)
			{
				underlyingReadCharacter = Utilities::seq_reverse_complement(underlyingReadCharacter);
			}
			assert(underlyingReadCharacter == sequenceCharacter);

			if(graphCharacter == "_")
			{
				totalMismatches++;
				weightedMismatches++;
			}
			else
			{
				// two well-defined characters
				char qualityCharacter = underlyingRead.quality.at(indexIntoOriginalReadData_correctlyAligned);
				double pCorrect = Utilities::PhredToPCorrect(qualityCharacter);
				assert((pCorrect > 0) && (pCorrect <= 1));
				if(sequenceCharacter == graphCharacter)
				{
					// match!
				}
				else
				{
					weightedMismatches += pCorrect;
					totalMismatches++;
				}
			}

		}
		else
		{
			assert(sequenceCharacter == "_");
			if(graphCharacter == "_")
			{
				// sequence gap, graph gap - likelihood 1
				assert(graphLevel != -1);
			}
			else
			{
				// sequence gap, graph non gap - deletion in sequence
				totalMismatches++;
				weightedMismatches++;
			}
		}
	}

	double readLength = underlyingRead.sequence.length();
	assert(totalMismatches >= weightedMismatches);

	return  (1 - (weightedMismatches / readLength));
};


double read_likelihood_per_position(const std::string& exonGenotype, const std::string& readGenotype, const std::string& readQualities, const int& alignmentGraphLevel)
{

	double log_likelihood = 0;
	bool verbose = 0;

	assert(exonGenotype.length() == 1);
	assert(readGenotype.length() >= 1);
	unsigned int l_diff = readGenotype.length() - exonGenotype.length();
	if(exonGenotype == "_")
	{
		// assert(l_diff == 0);
		if(readGenotype == "_")
		{
			assert(alignmentGraphLevel != -1);
			// likelihood 1 - intrinsic graph gap

			if(verbose)
			{
				std::cout << "\t\t" << "Intrinsic graph gap" << "\n";
			}

		}
		else
		{
			if(verbose)
			{
				std::cout << "\t\t" << "Insertion " << (1 + l_diff) << "\n";
			}

			log_likelihood += (log_likelihood_insertion * (1 + l_diff));
		}
	}
	else
	{

		// score from first position match
		if(readGenotype.substr(0, 1) == "_")
		{
			log_likelihood += log_likelihood_deletion;

			if(verbose)
			{
				std::cout << "\t\t" << "Deletion" << "\n";
			}
		}
		else
		{
			assert(readQualities.length());
			double pCorrect = Utilities::PhredToPCorrect(readQualities.at(0));
			assert((pCorrect > 0) && (pCorrect <= 1));

			if(!(pCorrect >= 0.25))
			{
				std::cerr << "pCorrect = " << pCorrect << "\n" << std::flush;
			}
			assert(pCorrect >= 0.25);

			if(exonGenotype == readGenotype.substr(0, 1))
			{
				if(verbose)
				{
					std::cout << "\t\t" << "Match " << pCorrect << "\n";
				}

				log_likelihood += log(pCorrect);
			}
			else
			{
				double pIncorrect = (1 - pCorrect)*(1.0/3.0);
				assert(pIncorrect <= 0.75);
				assert((pIncorrect > 0) && (pIncorrect < 1));
				log_likelihood += log(pIncorrect);

				if(verbose)
				{
					std::cout << "\t\t" << "Mismatch " << pIncorrect << "\n";
				}
			}
		}
		// if read allele is longer
		log_likelihood += (log_likelihood_insertion * l_diff);

		if(l_diff > 0)
		{
			if(verbose)
			{
				std::cout << "\t\t" << "Insertion " << l_diff << "\n";
			}
		}
	}

	if(verbose)
	{
		std::cout << "\t\t" << "Running log likelihood: " << log_likelihood << "\n";
	}

	return log_likelihood;
}

void simulateHLAreads_perturbHaplotype(std::vector<std::string>& haplotype)
{
	double rate_perturbation = 0.01;
	for(unsigned int pI = 0; pI < haplotype.size(); pI++)
	{
		if(Utilities::randomDouble() >= (1 - rate_perturbation))
		{
			int type_of_event = Utilities::randomNumber(10);

			if(type_of_event <= 5)
			{
				// SNV
				std::string newAllele;
				newAllele.push_back(Utilities::randomNucleotide());
				haplotype.at(pI) = newAllele;
			}
			else if((type_of_event > 5) && (type_of_event <= 8))
			{
				// deletion
				haplotype.at(pI) = "_";
			}
			else
			{
				// insertion
				std::string newAllele = haplotype.at(pI);
				int length = Utilities::randomNumber(2);
				for(unsigned int l = 0; length <= length; l++)
				{
					newAllele.push_back(Utilities::randomNucleotide());
				}
				haplotype.at(pI) = newAllele;
			}
		}
	}
}

void simulateHLAreads(std::string graphDir, int nIndividuals, bool perturbHaplotypes, std::string outputDirectory, std::string qualityMatrixFile, double insertSize_mean, double insertSize_sd, double haploidCoverage)
{
	std::string graph = graphDir + "/graph.txt";
	assert(Utilities::fileReadable(graph));

	if(! Utilities::directoryExists(outputDirectory))
	{
		Utilities::makeDir(outputDirectory);
	}

	std::set<std::string> loci;
	std::map<std::string, std::vector<std::string> > files_per_locus;
	std::map<std::string, std::vector<std::string> > files_per_locus_type;

	// find files and loci

	std::vector<std::string> files_in_order;
	std::vector<std::string> files_in_order_type;
	std::vector<int> files_in_order_number;

	std::ifstream segmentsStream;
	std::string segmentsFileName = graphDir + "/segments.txt";
	segmentsStream.open(segmentsFileName.c_str());
	assert(segmentsStream.is_open());
	std::string line;
	while(segmentsStream.good())
	{
		std::getline(segmentsStream, line);
		Utilities::eraseNL(line);
		if(line.length() > 0)
		{
			std::vector<std::string> split_by_underscore = Utilities::split(line, "_");
			std::string file_locus = split_by_underscore.at(0);
			loci.insert(file_locus);

			files_per_locus[file_locus].push_back(line);
			files_per_locus_type[file_locus].push_back(split_by_underscore.at(2));
		}
	}
	assert(loci.size() > 0);


	std::cout << "simulateHLAreads(..): Found " << loci.size() << "loci.\n";

	// find available types

	std::map<std::string, std::vector<std::string> > loci_availableTypes;
	for(std::set<std::string>::iterator locusIt = loci.begin(); locusIt != loci.end(); locusIt++)
	{
		std::string locus = *locusIt;

		std::string arbitraryIntronFile;
		for(unsigned int fI = 0; fI < files_per_locus.at(locus).size(); fI++)
		{
			if(files_per_locus_type.at(locus).at(fI) == "intron")
			{
				arbitraryIntronFile = files_per_locus.at(locus).at(fI);
			}
		}
		assert(arbitraryIntronFile.length());

		std::vector<std::string> availableTypes;

		std::ifstream fileInputStream;
		fileInputStream.open(arbitraryIntronFile.c_str());
		assert(fileInputStream.is_open());
		unsigned int lI = 0;
		while(fileInputStream.good())
		{
			std::string line;
			std::getline(fileInputStream, line);
			Utilities::eraseNL(line);
			std::vector<std::string> line_fields = Utilities::split(line, " ");
			if(lI == 0)
			{
				assert(line_fields.at(0) == "IndividualID");
			}
			else
			{
				std::string type = line_fields.at(0);
				availableTypes.push_back(type);
			}
			lI++;
		}
		fileInputStream.close();

		loci_availableTypes[locus] = availableTypes;
	}


	// find type haplotypes

	std::map<std::string, std::map<std::string, std::vector<std::string>> > loci_types_haplotypes;
	std::map<std::string, std::vector<std::string> > loci_graphLevelIDs;

	for(std::set<std::string>::iterator locusIt = loci.begin(); locusIt != loci.end(); locusIt++)
	{
		std::string locus = *locusIt;
		std::set<std::string> availableTypes_set(loci_availableTypes.at(locus).begin(), loci_availableTypes.at(locus).end());

		for(unsigned int fI = 0; fI < files_per_locus.at(locus).size(); fI++)
		{
			std::ifstream fileInputStream;
			fileInputStream.open(files_per_locus.at(locus).at(fI).c_str());
			assert(fileInputStream.is_open());
			std::vector<std::string> file_lines;
			while(fileInputStream.good())
			{
				std::string line;
				std::getline(fileInputStream, line);
				Utilities::eraseNL(line);
				file_lines.push_back(line);
			}
			fileInputStream.close();

			std::string firstLine = file_lines.at(0);
			std::vector<std::string> firstLine_fields = Utilities::split(firstLine, " ");
			assert(firstLine_fields.at(0) == "IndividualID");

			std::vector<std::string> graph_level_names(firstLine_fields.begin() + 1, firstLine_fields.end());
			loci_graphLevelIDs[locus].insert(loci_graphLevelIDs[locus].end(), graph_level_names.begin(), graph_level_names.end());

			for(unsigned int lI = 1; lI < file_lines.size(); lI++)
			{
				if(file_lines.at(lI).length())
				{
					std::vector<std::string> line_fields = Utilities::split(file_lines.at(lI), " ");
					assert(line_fields.size() == firstLine_fields.size());
					std::string HLA_type = line_fields.at(0);
					std::vector<std::string> line_alleles(line_fields.begin()+1, line_fields.end());

					if((files_per_locus_type.at(locus).at(fI) == "intron") || (files_per_locus_type.at(locus).at(fI) == "exon"))
					{
						if(availableTypes_set.count(HLA_type))
						{
							if(loci_types_haplotypes[locus].count(HLA_type) == 0)
							{
								loci_types_haplotypes[locus][HLA_type].resize(0);
							}

							loci_types_haplotypes.at(locus).at(HLA_type).insert(loci_types_haplotypes.at(locus).at(HLA_type).end(), line_alleles.begin(), line_alleles.end());
						}
					}
					else
					{
						// this must be a pre- or post-padding sequence
						std::cout << "TYPE: " << files_per_locus_type.at(locus).at(fI) << "\n";

						for(std::set<std::string>::iterator typeIt = availableTypes_set.begin(); typeIt != availableTypes_set.end(); typeIt++)
						{
							std::string HLA_type = *typeIt;
							if(loci_types_haplotypes[locus].count(HLA_type) == 0)
							{
								loci_types_haplotypes[locus][HLA_type].resize(0);
							}

							loci_types_haplotypes.at(locus).at(HLA_type).insert(loci_types_haplotypes.at(locus).at(HLA_type).end(), line_alleles.begin(), line_alleles.end());
						}

						break;
					}
				}
			}
		}
	}

	// check that all haplotypes make sense

	for(std::set<std::string>::iterator locusIt = loci.begin(); locusIt != loci.end(); locusIt++)
	{
		std::string locus = *locusIt;
		assert(loci_availableTypes.at(locus).size() > 0);
		for(unsigned int tI = 0; tI < loci_availableTypes.at(locus).size(); tI++)
		{
			std::string HLA_type = loci_availableTypes.at(locus).at(tI);
			assert(loci_types_haplotypes.at(locus).at(HLA_type).size() == loci_graphLevelIDs.at(locus).size());
		}
	}

	// start simulations
	std::string outputFile_trueHLA = outputDirectory + "/trueHLA.txt";
	std::string outputFile_trueHaplotypes = outputDirectory + "/trueHaplotypes.txt";

	std::ofstream trueHLAstream;
	std::ofstream trueHaplotypesstream;

	trueHLAstream.open(outputFile_trueHLA.c_str());
	assert(trueHLAstream.is_open());
	trueHaplotypesstream.open(outputFile_trueHaplotypes.c_str());
	assert(trueHaplotypesstream.is_open());

	std::vector<std::string> truthFiles_headerFields;
	truthFiles_headerFields.push_back("IndividualID");
	truthFiles_headerFields.insert(truthFiles_headerFields.end(), loci.begin(), loci.end());
	trueHLAstream << Utilities::join(truthFiles_headerFields, " ") << "\n";
	trueHaplotypesstream << Utilities::join(truthFiles_headerFields, " ") << "\n";

	std::string outputFile_trueHaplotypes_fieldNames = outputDirectory + "/trueHaplotypes_fieldNames.txt";
	std::ofstream trueHaplotypes_fieldNames_stream;
	trueHaplotypes_fieldNames_stream.open(outputFile_trueHaplotypes_fieldNames.c_str());
	assert(trueHaplotypes_fieldNames_stream.is_open());
	for(std::set<std::string>::iterator locusIt = loci.begin(); locusIt != loci.end(); locusIt++)
	{
		std::vector<std::string> lineFields;
		lineFields.push_back(*locusIt);
		lineFields.insert(lineFields.end(), loci_graphLevelIDs.at(*locusIt).begin(), loci_graphLevelIDs.at(*locusIt).end());
		trueHaplotypes_fieldNames_stream << Utilities::join(lineFields, " ") << "\n";
	}
	trueHaplotypes_fieldNames_stream.close();

	std::string outputFile_simulationDetails = outputDirectory + "/simulationDetails.txt";
	std::ofstream simulationDetailsStream;
	simulationDetailsStream.open(outputFile_simulationDetails.c_str());
	assert(simulationDetailsStream.is_open());
	simulationDetailsStream << "nIndividuals" << " " << nIndividuals << "\n";
	simulationDetailsStream << "perturbHaplotypes" << " " << perturbHaplotypes << "\n";
	simulationDetailsStream << "haploidCoverage" << " " << haploidCoverage << "\n";
	simulationDetailsStream << "insertSize_mean" << " " << insertSize_mean << "\n";
	simulationDetailsStream << "insertSize_sd" << " " << insertSize_sd << "\n";
	simulationDetailsStream.close();

	readSimulator rS(qualityMatrixFile);

	for(int sI = 0; sI < nIndividuals; sI++)
	{
		std::cout << "simulateHLAreads(..): Individual " << sI << " / " << nIndividuals << "\n" << std::flush;

		std::string outputFile_FASTQ_1 = outputDirectory + "/S_" + Utilities::ItoStr(sI) + "_1.fastq";
		std::string outputFile_FASTQ_2 = outputDirectory + "/S_" + Utilities::ItoStr(sI) + "_2.fastq";

		std::ofstream fastQStream_1;
		fastQStream_1.open(outputFile_FASTQ_1.c_str());
		assert(fastQStream_1.is_open());

		std::ofstream fastQStream_2;
		fastQStream_2.open(outputFile_FASTQ_2.c_str());
		assert(fastQStream_2.is_open());

		std::vector<std::string> outputFields_trueHLA;
		std::vector<std::string> outputFields_trueHaplotypes;

		outputFields_trueHLA.push_back("S_" + Utilities::ItoStr(sI));
		outputFields_trueHaplotypes.push_back("S_" + Utilities::ItoStr(sI));

		for(std::set<std::string>::iterator locusIt = loci.begin(); locusIt != loci.end(); locusIt++)
		{
			std::string locus = *locusIt;
			std::string selectedType_1 = loci_availableTypes.at(locus).at(Utilities::randomNumber(loci_availableTypes.at(locus).size()));
			std::string selectedType_2 = loci_availableTypes.at(locus).at(Utilities::randomNumber(loci_availableTypes.at(locus).size()));

			std::cout << "\tLocus " << locus << " selected " << selectedType_1 << " / " << selectedType_2 << "\n" << std::flush;

			std::vector<std::string> haplotype_1 = loci_types_haplotypes.at(locus).at(selectedType_1);
			std::vector<std::string> haplotype_2 = loci_types_haplotypes.at(locus).at(selectedType_2);

			if(perturbHaplotypes)
			{
				simulateHLAreads_perturbHaplotype(haplotype_1);
				simulateHLAreads_perturbHaplotype(haplotype_2);
			}

			std::string haplotype_1_noGaps = Utilities::join(haplotype_1, "");
			std::string haplotype_2_noGaps = Utilities::join(haplotype_2, "");

			haplotype_1_noGaps.erase(std::remove_if(haplotype_1_noGaps.begin(),haplotype_1_noGaps.end(), [&](char c){return ((c == '_') ? true : false);}), haplotype_1_noGaps.end());
			haplotype_2_noGaps.erase(std::remove_if(haplotype_2_noGaps.begin(),haplotype_2_noGaps.end(), [&](char c){return ((c == '_') ? true : false);}), haplotype_2_noGaps.end());

			std::vector<oneReadPair> simulatedReadPairs_h1 = rS.simulate_paired_reads_from_string(haplotype_1_noGaps, haploidCoverage, insertSize_mean, insertSize_sd, false);
			std::vector<oneReadPair> simulatedReadPairs_h2 = rS.simulate_paired_reads_from_string(haplotype_2_noGaps, haploidCoverage, insertSize_mean, insertSize_sd, false);

			auto print_one_readPair = [] (oneReadPair& rP, std::ofstream& output_1, std::ofstream& output_2) -> void
			{
				output_1 << "@" << rP.reads.first.name << "\n";
				output_1 << rP.reads.first.sequence << "\n";
				output_1 << "+" << "\n";
				output_1 << rP.reads.first.quality << "\n";

				output_2 << "@" << rP.reads.second.name << "\n";
				output_2 << rP.reads.second.sequence << "\n";
				output_2 << "+" << "\n";
				output_2 << rP.reads.second.quality << "\n";
			};

			for(unsigned int pI = 0; pI < simulatedReadPairs_h1.size(); pI++)
			{
				print_one_readPair(simulatedReadPairs_h1.at(pI), fastQStream_1, fastQStream_2);
			}

			for(unsigned int pI = 0; pI < simulatedReadPairs_h2.size(); pI++)
			{
				print_one_readPair(simulatedReadPairs_h2.at(pI), fastQStream_1, fastQStream_2);
			}

			outputFields_trueHLA.push_back(selectedType_1 + "/" + selectedType_2);
			outputFields_trueHaplotypes.push_back(Utilities::join(haplotype_1, ";") + "/" + Utilities::join(haplotype_2, ";"));
		}

		trueHLAstream << Utilities::join(outputFields_trueHLA, " ") << "\n" << std::flush;
		trueHaplotypesstream << Utilities::join(outputFields_trueHaplotypes, " ") << "\n" << std::flush;

		fastQStream_1.close();
		fastQStream_2.close();
	}

	trueHLAstream.close();
	trueHaplotypesstream.close();
}

void HLAHaplotypeInference(std::string alignedReads_file, std::string graphDir, std::string sampleName, std::string loci_str, std::string starting_haplotypes_perLocus_1_str, std::string starting_haplotypes_perLocus_2_str)
{
	std::string graph = graphDir + "/graph.txt";
	assert(Utilities::fileReadable(graph));

	std::vector<std::string> loci = Utilities::split(loci_str, ",");
	std::vector<std::string> starting_haplotypes_perLocus_1 = Utilities::split(starting_haplotypes_perLocus_1_str, ",");
	std::vector<std::string> starting_haplotypes_perLocus_2 = Utilities::split(starting_haplotypes_perLocus_2_str, ",");
	assert(loci.size() == starting_haplotypes_perLocus_1.size());
	assert(loci.size() == starting_haplotypes_perLocus_2.size());

	// translate location IDs to graph levels
	std::vector<std::string> graphLoci = readGraphLoci(graphDir);
	std::map<std::string, unsigned int> graphLocus_2_levels;
	for(unsigned int i = 0; i < graphLoci.size(); i++)
	{
		std::string locusID = graphLoci.at(i);
		assert(graphLocus_2_levels.count(locusID) == 0);
		graphLocus_2_levels[locusID] = i;
	}

	// load reads
	std::cout << Utilities::timestamp() << "HLAHaplotypeInference(..): Load reads.\n" << std::flush;
	std::vector<std::pair<seedAndExtend_return_local, seedAndExtend_return_local>> alignments;
	std::vector<oneReadPair> alignments_originalReads;

	double insertSize_mean;
	double insertSize_sd;

	read_shortReadAlignments_fromFile(alignedReads_file, alignments, alignments_originalReads, insertSize_mean, insertSize_sd);
	assert(alignments.size() == alignments_originalReads.size());

	std::cout << Utilities::timestamp() << "HLAHaplotypeInference(..): Load reads -- done. Have " << alignments.size() << " read pairs, IS mean " << insertSize_mean << " / sd " << insertSize_sd << ".\n" << std::flush;

	// read alignment statistics

	int alignmentStats_strandsValid = 0;
	int alignments_perfect = 0;
	int alignments_oneReadPerfect = 0;
	int alignmentStats_strandsValid_and_distanceOK = 0;
	std::vector<double> alignmentStats_strandsValid_distances;
	double alignmentStats_fractionOK_sum = 0;
	for(unsigned int alignmentI = 0; alignmentI < alignments.size(); alignmentI++)
	{

		std::pair<seedAndExtend_return_local, seedAndExtend_return_local>& alignedReadPair = alignments.at(alignmentI);

		if(alignedReadPair_strandsValid(alignedReadPair))
		{
			alignmentStats_strandsValid++;
			double pairsDistance = alignedReadPair_pairsDistanceInGraphLevels(alignedReadPair);
			alignmentStats_strandsValid_distances.push_back(pairsDistance);
			if(abs(pairsDistance - insertSize_mean) <= (5 * insertSize_sd))
			{
				alignmentStats_strandsValid_and_distanceOK++;
			}
		}

		double fractionOK_1 = alignmentFractionOK(alignedReadPair.first);
		double fractionOK_2 = alignmentFractionOK(alignedReadPair.second);

		if(fractionOK_1 == 1)
		{
			alignments_perfect++;
		}
		if(fractionOK_2 == 1)
		{
			alignments_perfect++;
		}
		if((fractionOK_1 == 1) || (fractionOK_2 == 1))
		{
			alignments_oneReadPerfect++;
		}

		alignmentStats_fractionOK_sum += fractionOK_1;
		alignmentStats_fractionOK_sum += fractionOK_2;
	}

	// std::pair<double, double> alignmentStats_distance_meanMedian = meanMedian(alignmentStats_strandsValid_distances);
	// double alignmentStats_fractionOK_avg = (alignments.size() > 0) ? (alignmentStats_fractionOK_sum / (2.0* (double)alignments.size())) : 0;

	if(! Utilities::directoryExists("../tmp/hla"))
	{
		Utilities::makeDir("../tmp/hla");
	}

	std::string outputDirectory = "../tmp/hla/"+sampleName;
	if(! Utilities::directoryExists(outputDirectory))
	{
		Utilities::makeDir(outputDirectory);
	}

	for(unsigned int locusI = 0; locusI < loci.size(); locusI++)
	{
		std::string starting_haplotypes_1_str = starting_haplotypes_perLocus_1.at(locusI);
		std::string starting_haplotypes_2_str = starting_haplotypes_perLocus_2.at(locusI);
		std::vector<std::string> starting_haplotypes_1_vec = Utilities::split(starting_haplotypes_1_str, ";");
		std::vector<std::string> starting_haplotypes_2_vec = Utilities::split(starting_haplotypes_2_str, ";");

		std::set<std::string> starting_haplotypes_1_set(starting_haplotypes_1_vec.begin(), starting_haplotypes_1_vec.end());
		std::set<std::string> starting_haplotypes_2_set(starting_haplotypes_2_vec.begin(), starting_haplotypes_2_vec.end());
		std::set<std::string> starting_haplotypes_combined_set = starting_haplotypes_1_set;
		starting_haplotypes_combined_set.insert(starting_haplotypes_2_set.begin(), starting_haplotypes_2_set.end());
		assert(starting_haplotypes_1_set.size() > 0);
		assert(starting_haplotypes_2_set.size() > 0);

		std::string locus = loci.at(locusI);
		std::cout << Utilities::timestamp() << "HLAHaplotypeInference(..): Making inference for " << locus << ", have " << starting_haplotypes_combined_set.size() << " combined starting haplotypes\n" << std::flush;

		// find intron and exon files belonging to locus
		std::vector<std::string> files_in_order;
		std::vector<std::string> files_in_order_type;
		std::vector<int> files_in_order_number;

		std::ifstream segmentsStream;
		std::string segmentsFileName = graphDir + "/segments.txt";
		segmentsStream.open(segmentsFileName.c_str());
		assert(segmentsStream.is_open());
		std::string line;
		while(segmentsStream.good())
		{
			std::getline(segmentsStream, line);
			Utilities::eraseNL(line);
			if(line.length() > 0)
			{
				std::vector<std::string> split_by_underscore = Utilities::split(line, "_");
				std::string file_locus = split_by_underscore.at(0);
				if(file_locus == locus)
				{
					if((split_by_underscore.at(2) == "intron") || (split_by_underscore.at(2) == "exon"))
					{
						files_in_order.push_back(line);
						files_in_order_type.push_back(split_by_underscore.at(2));
						files_in_order_number.push_back(Utilities::StrtoI(split_by_underscore.at(3)));
					}
				}
			}
		}
		assert(files_in_order.size() > 0);

		std::cout << "HLAHaplotypeInference(..): Files read in for locus " << locus << "\n";
		std::cout << Utilities::join(files_in_order, ",   ") << "\n\n" << std::flush;


		std::vector<int> combined_sequences_graphLevels;
		std::vector<std::string> combined_sequences_graphLevels_individualType;
		std::vector<int> combined_sequences_graphLevels_individualTypeNumber;
		std::vector<int> combined_sequences_graphLevels_individualPosition;

		std::vector<std::string> combined_sequences_locusIDs;
		std::map<std::string, std::string> combined_sequences;

		for(unsigned int fileI = 0; fileI < files_in_order.size(); fileI++)
		{
			std::cout << Utilities::timestamp() << "\tLocus" << locus << ", file " << fileI << "\n" << std::flush;
			std::string file = files_in_order.at(fileI);

			std::string type = files_in_order_type.at(fileI);
			int typeNumber = files_in_order_number.at(fileI);

			if(! Utilities::fileReadable(file))
			{
				std::cerr << "HLAHaplotypeInference(..): Locus " << locus << ", fileI " << fileI << ": Can't read file " << file << "\n";
			}
			assert(Utilities::fileReadable(file));

			std::ifstream fileInputStream;
			fileInputStream.open(file.c_str());
			assert(fileInputStream.is_open());
			std::vector<std::string> file_lines;
			while(fileInputStream.good())
			{
				std::string line;
				std::getline(fileInputStream, line);
				Utilities::eraseNL(line);
				file_lines.push_back(line);
			}
			fileInputStream.close();

			std::string firstLine = file_lines.at(0);
			std::vector<std::string> firstLine_fields = Utilities::split(firstLine, " ");
			assert(firstLine_fields.at(0) == "IndividualID");

			std::vector<std::string> exon_level_names(firstLine_fields.begin() + 1, firstLine_fields.end());
			std::string first_graph_locusID = exon_level_names.front();
			std::string last_graph_locusID = exon_level_names.back();

			assert(graphLocus_2_levels.count(first_graph_locusID));
			assert(graphLocus_2_levels.count(last_graph_locusID));


			unsigned int first_graph_level = graphLocus_2_levels.at(first_graph_locusID);
			unsigned int last_graph_level = graphLocus_2_levels.at(last_graph_locusID);

			std::cout << Utilities::timestamp() << "\tLocus" << locus << ", fileI " << fileI << ": from " << first_graph_locusID << " (" << first_graph_level << ") to " << last_graph_locusID << " (" << last_graph_level << ").\n" << std::flush;

			assert(last_graph_level > first_graph_level);
			unsigned int expected_allele_length = last_graph_level - first_graph_level + 1;
			if(!(exon_level_names.size() == expected_allele_length))
			{
				std::cerr << "For locus " << locus << " fileI " << fileI << " (" << file << "), we have a problem with expected graph length.\n";
				std::cerr << "\t" << "expected_allele_length" << ": " << expected_allele_length << "\n";
				std::cerr << "\t" << "expected_allele_length" << ": " << expected_allele_length << "\n";
				std::cerr << std::flush;
			}
			assert(exon_level_names.size() == expected_allele_length);

			combined_sequences_locusIDs.insert(combined_sequences_locusIDs.end(), exon_level_names.begin(), exon_level_names.end());
			for(unsigned int lI = 0; lI < expected_allele_length; lI++)
			{
				unsigned int graphLevel = first_graph_level + lI;
				assert(graphLocus_2_levels.at(exon_level_names.at(lI)) == graphLevel);
				combined_sequences_graphLevels.push_back(graphLevel);
				combined_sequences_graphLevels_individualType.push_back(type);
				combined_sequences_graphLevels_individualTypeNumber.push_back(typeNumber);
				combined_sequences_graphLevels_individualPosition.push_back(lI);
			}


			for(unsigned int lI = 1; lI < file_lines.size(); lI++)
			{
				if(file_lines.at(lI).length())
				{
					std::vector<std::string> line_fields = Utilities::split(file_lines.at(lI), " ");
					assert(line_fields.size() == firstLine_fields.size());
					std::string HLA_type = line_fields.at(0);
					std::vector<std::string> line_alleles(line_fields.begin()+1, line_fields.end());

					std::string HLA_type_sequence = Utilities::join(line_alleles, "");

					if(fileI == 0)
					{
						assert(combined_sequences.count(HLA_type) == 0);
						combined_sequences[HLA_type] = HLA_type_sequence;
					}
					else
					{
						assert(combined_sequences.count(HLA_type));
						combined_sequences.at(HLA_type) += HLA_type_sequence;
					}
				}
			}
		}

		for(std::set<std::string>::iterator haplotypeIt = starting_haplotypes_combined_set.begin(); haplotypeIt != starting_haplotypes_combined_set.end(); haplotypeIt++)
		{
			std::string haplotypeID = *haplotypeIt;
			assert(combined_sequences.count(haplotypeID) > 0);
			assert(combined_sequences.at(haplotypeID).length() == combined_sequences_locusIDs.size());
		}

		std::map<int, unsigned int> graphLevel_2_position;
		std::map<int, std::string> graphLevel_2_position_type;
		std::map<int, unsigned int> graphLevel_2_exonPosition_typeNumber;
		std::map<int, unsigned int> graphLevel_2_exonPosition_position;
		for(unsigned int pI = 0; pI < combined_sequences_graphLevels.size(); pI++)
		{
			int graphLevel = combined_sequences_graphLevels.at(pI);
			assert(graphLevel >= 0);
			graphLevel_2_position[graphLevel] = pI;
			graphLevel_2_position_type[graphLevel] = combined_sequences_graphLevels_individualType.at(pI);
			graphLevel_2_exonPosition_typeNumber[graphLevel] = combined_sequences_graphLevels_individualTypeNumber.at(pI);
			graphLevel_2_exonPosition_position[graphLevel] = combined_sequences_graphLevels_individualPosition.at(pI);

		}

		std::cout << Utilities::timestamp() << "Have collected " << combined_sequences.size() << " sequences -- first level " << combined_sequences_graphLevels.front() << ", last level " << combined_sequences_graphLevels.back() << ".\n" << std::flush;

		// now transform reads into sequences specifying genotype value

		std::cout << Utilities::timestamp() << "Compute exon positions and specified genotypes from reads\n" << std::flush;

		std::vector< std::vector<oneExonPosition> > positions_fromReads;

		auto oneReadAlignment_2_exonPositions = [&](seedAndExtend_return_local& alignment, oneRead& read, std::vector<oneExonPosition>& ret_exonPositions, seedAndExtend_return_local& paired_alignment, oneRead& paired_read) -> void {
			int alignment_firstLevel = alignment.alignment_firstLevel();
			int alignment_lastLevel = alignment.alignment_lastLevel();

			double thisRead_fractionOK = alignmentFractionOK(alignment);
			double pairedRead_fractionOK = alignmentFractionOK(paired_alignment);

			double thisRead_WeightedCharactersOK = alignmentWeightedOKFraction(read, alignment);
			double pairedRead_WeightedCharactersOK = alignmentWeightedOKFraction(paired_read, paired_alignment);

			std::pair<seedAndExtend_return_local, seedAndExtend_return_local> alignedReadPair = make_pair(alignment, paired_alignment);
			double pairs_strands_OK = alignedReadPair_strandsValid(alignedReadPair);
			double pairs_strands_distance = alignedReadPair_pairsDistanceInGraphLevels(alignedReadPair);

			// std::cout << "This alignment " << alignment_firstLevel << " - " << alignment_lastLevel << "\n";
			// std::cout << "\tvs combined exon " << combined_exon_sequences_graphLevels.front() << " - " << combined_exon_sequences_graphLevels.back() << "\n\n" << std::flush;

			if( ((alignment_firstLevel >= combined_sequences_graphLevels.front()) && (alignment_firstLevel <= combined_sequences_graphLevels.back())) ||
				((alignment_lastLevel >= combined_sequences_graphLevels.front()) && (alignment_lastLevel <= combined_sequences_graphLevels.back())) )
			{
				std::vector<oneExonPosition> readAlignment_exonPositions;

				int indexIntoOriginalReadData = -1;
				for(unsigned int cI = 0; cI < alignment.sequence_aligned.length(); cI++)
				{
					std::string sequenceCharacter = alignment.sequence_aligned.substr(cI, 1);
					std::string graphCharacter = alignment.graph_aligned.substr(cI, 1);
					int graphLevel = alignment.graph_aligned_levels.at(cI);

					if(graphLevel == -1)
					{
						// insertion relative to the graph - we need to extend last character

						assert(graphCharacter == "_");
						assert(sequenceCharacter != "_");

						indexIntoOriginalReadData++;
						int indexIntoOriginalReadData_correctlyAligned = indexIntoOriginalReadData;
						if(alignment.reverse)
						{
							indexIntoOriginalReadData_correctlyAligned = read.sequence.length() - indexIntoOriginalReadData_correctlyAligned - 1;
						}
						assert(indexIntoOriginalReadData_correctlyAligned >= 0);
						assert(indexIntoOriginalReadData_correctlyAligned < (int)read.sequence.length());;

						std::string underlyingReadCharacter = read.sequence.substr(indexIntoOriginalReadData_correctlyAligned, 1);
						if(alignment.reverse)
						{
							underlyingReadCharacter = Utilities::seq_reverse_complement(underlyingReadCharacter);
						}
						assert(underlyingReadCharacter == sequenceCharacter);
						char qualityCharacter = read.quality.at(indexIntoOriginalReadData_correctlyAligned);

						if(readAlignment_exonPositions.size() > 0)
						{
							readAlignment_exonPositions.back().genotype.append(sequenceCharacter);
							readAlignment_exonPositions.back().alignment_edgelabels.append(graphCharacter);
							readAlignment_exonPositions.back().qualities.push_back(qualityCharacter);
							assert(readAlignment_exonPositions.back().genotype.length() == readAlignment_exonPositions.back().qualities.length());
							assert(readAlignment_exonPositions.back().alignment_edgelabels.length() == readAlignment_exonPositions.back().genotype.length());
						}
					}
					else
					{
						if(sequenceCharacter != "_")
						{
							indexIntoOriginalReadData++;
							int indexIntoOriginalReadData_correctlyAligned = indexIntoOriginalReadData;
							if(alignment.reverse)
							{
								indexIntoOriginalReadData_correctlyAligned = read.sequence.length() - indexIntoOriginalReadData_correctlyAligned - 1;
							}
							assert(indexIntoOriginalReadData_correctlyAligned >= 0);
							assert(indexIntoOriginalReadData_correctlyAligned < (int)read.sequence.length());

							std::string underlyingReadCharacter = read.sequence.substr(indexIntoOriginalReadData_correctlyAligned, 1);
							if(alignment.reverse)
							{
								underlyingReadCharacter = Utilities::seq_reverse_complement(underlyingReadCharacter);
							}
							assert(underlyingReadCharacter == sequenceCharacter);

							if(graphCharacter == "_")
							{

								assert(graphLevel != -1);

								char qualityCharacter = read.quality.at(indexIntoOriginalReadData_correctlyAligned);

								oneExonPosition thisPosition;
								thisPosition.graphLevel = graphLevel;
								thisPosition.genotype = sequenceCharacter;
								thisPosition.alignment_edgelabels = graphCharacter;
								thisPosition.qualities.push_back(qualityCharacter);

								thisPosition.thisRead_ID = read.name;
								thisPosition.pairedRead_ID = paired_read.name;
								thisPosition.thisRead_fractionOK = thisRead_fractionOK;
								thisPosition.pairedRead_fractionOK = pairedRead_fractionOK;
								thisPosition.pairs_strands_OK = pairs_strands_OK;
								thisPosition.pairs_strands_distance = pairs_strands_distance;
								thisPosition.thisRead_WeightedCharactersOK = thisRead_WeightedCharactersOK;
								thisPosition.pairedRead_WeightedCharactersOK = pairedRead_WeightedCharactersOK;

								readAlignment_exonPositions.push_back(thisPosition);

							}
							else
							{
								// two well-defined characters
								char qualityCharacter = read.quality.at(indexIntoOriginalReadData_correctlyAligned);

								oneExonPosition thisPosition;
								thisPosition.graphLevel = graphLevel;
								thisPosition.genotype = sequenceCharacter;
								thisPosition.alignment_edgelabels = graphCharacter;
								thisPosition.qualities.push_back(qualityCharacter);

								thisPosition.thisRead_ID = read.name;
								thisPosition.pairedRead_ID = paired_read.name;
								thisPosition.thisRead_fractionOK = thisRead_fractionOK;
								thisPosition.pairedRead_fractionOK = pairedRead_fractionOK;
								thisPosition.pairs_strands_OK = pairs_strands_OK;
								thisPosition.pairs_strands_distance = pairs_strands_distance;
								thisPosition.thisRead_WeightedCharactersOK = thisRead_WeightedCharactersOK;
								thisPosition.pairedRead_WeightedCharactersOK = pairedRead_WeightedCharactersOK;

								readAlignment_exonPositions.push_back(thisPosition);
							}

						}
						else
						{
							assert(sequenceCharacter == "_");
							if(graphCharacter == "_")
							{
								assert(graphLevel != -1);

								oneExonPosition thisPosition;
								thisPosition.graphLevel = graphLevel;
								thisPosition.genotype = "_";
								thisPosition.alignment_edgelabels = graphCharacter;
								thisPosition.qualities = "";

								thisPosition.thisRead_ID = read.name;
								thisPosition.pairedRead_ID = paired_read.name;
								thisPosition.thisRead_fractionOK = thisRead_fractionOK;
								thisPosition.pairedRead_fractionOK = pairedRead_fractionOK;
								thisPosition.pairs_strands_OK = pairs_strands_OK;
								thisPosition.pairs_strands_distance = pairs_strands_distance;
								thisPosition.thisRead_WeightedCharactersOK = thisRead_WeightedCharactersOK;
								thisPosition.pairedRead_WeightedCharactersOK = pairedRead_WeightedCharactersOK;

								readAlignment_exonPositions.push_back(thisPosition);
							}
							else
							{
								oneExonPosition thisPosition;
								thisPosition.graphLevel = graphLevel;
								thisPosition.genotype = "_";
								thisPosition.alignment_edgelabels = graphCharacter;
								thisPosition.qualities = "";

								thisPosition.thisRead_ID = read.name;
								thisPosition.pairedRead_ID = paired_read.name;
								thisPosition.thisRead_fractionOK = thisRead_fractionOK;
								thisPosition.pairedRead_fractionOK = pairedRead_fractionOK;
								thisPosition.pairs_strands_OK = pairs_strands_OK;
								thisPosition.pairs_strands_distance = pairs_strands_distance;
								thisPosition.thisRead_WeightedCharactersOK = thisRead_WeightedCharactersOK;
								thisPosition.pairedRead_WeightedCharactersOK = pairedRead_WeightedCharactersOK;

								readAlignment_exonPositions.push_back(thisPosition);
							}
						}
					}
				}

				int alongReadMode = 0;
				int lastPositionInExon = -1;
				for(unsigned int posInAlignment = 0; posInAlignment < readAlignment_exonPositions.size(); posInAlignment++)
				{
					oneExonPosition& thisPosition = readAlignment_exonPositions.at(posInAlignment);
					assert(thisPosition.graphLevel != -1);
					if(graphLevel_2_position.count(thisPosition.graphLevel))
					{
						assert((alongReadMode == 0) || (alongReadMode == 1));
						thisPosition.positionInExon = graphLevel_2_position.at(thisPosition.graphLevel);
						assert((lastPositionInExon == -1) || ((int)thisPosition.positionInExon == (lastPositionInExon + 1)));
						lastPositionInExon = thisPosition.positionInExon;
						alongReadMode = 1;

						ret_exonPositions.push_back(thisPosition);
					}
					else
					{
						if(alongReadMode == 1)
						{
							alongReadMode = 2;
						}
					}
				}
			}

		};

		unsigned int readPairs_OK = 0;
		unsigned int readPairs_broken = 0;

		for(unsigned int readPairI = 0; readPairI < alignments.size(); readPairI++)
		{
			oneReadPair& originalReadPair = alignments_originalReads.at(readPairI);
			std::pair<seedAndExtend_return_local, seedAndExtend_return_local>& alignedReadPair = alignments.at(readPairI);

			std::vector<oneExonPosition> read1_positions;
			std::vector<oneExonPosition> read2_positions;
			oneReadAlignment_2_exonPositions(alignedReadPair.first, originalReadPair.reads.first, read1_positions, alignedReadPair.second, originalReadPair.reads.second);
			oneReadAlignment_2_exonPositions(alignedReadPair.second, originalReadPair.reads.second, read2_positions, alignedReadPair.first, originalReadPair.reads.first);

			if(
					alignedReadPair_strandsValid(alignedReadPair) &&
					(abs(alignedReadPair_pairsDistanceInGraphLevels(alignedReadPair) - insertSize_mean) <= (5 * insertSize_sd)) &&
					(alignmentFractionOK(alignedReadPair.first) >= min_alignmentFraction_OK) &&
					(alignmentFractionOK(alignedReadPair.second) >= min_alignmentFraction_OK) &&
					((alignmentWeightedOKFraction(originalReadPair.reads.first, alignedReadPair.first) >= min_oneRead_weightedCharactersOK) || (alignmentWeightedOKFraction(originalReadPair.reads.second, alignedReadPair.second) >= min_oneRead_weightedCharactersOK))
			)
			{
				// good

				// std::cout << "\t\t" << "readPair " << readPairI << ", pairing OK.\n" << std::flush;

				std::vector<oneExonPosition> thisRead_positions = read1_positions;
				thisRead_positions.insert(thisRead_positions.end(), read2_positions.begin(), read2_positions.end());
				if(thisRead_positions.size() > 0)
					positions_fromReads.push_back(thisRead_positions);

				readPairs_OK++;
			}
		}

		std::cout << Utilities::timestamp() << "Mapped reads to exons. " << readPairs_OK << " pairs OK, " << readPairs_broken << " pairs broken." << "\n" << std::flush;

		// Pileup of mapped reads

		std::vector<std::vector<oneExonPosition> > pileUpPerPosition;
		pileUpPerPosition.resize(combined_sequences_graphLevels.size());
		for(unsigned int positionSpecifierI = 0; positionSpecifierI < positions_fromReads.size(); positionSpecifierI++)
		{
			std::vector<oneExonPosition>& individualPositions = positions_fromReads.at(positionSpecifierI);
			for(unsigned int positionI = 0; positionI < individualPositions.size(); positionI++)
			{
				oneExonPosition& onePositionSpecifier = individualPositions.at(positionI);

				unsigned int position = graphLevel_2_position.at(onePositionSpecifier.graphLevel);
				pileUpPerPosition.at(position).push_back(onePositionSpecifier);
			}
		}

		std::string fileName_pileUp = outputDirectory + "/R1_pileup_"+locus+".txt";
		std::ofstream pileUpStream;
		pileUpStream.open(fileName_pileUp.c_str());
		assert(pileUpStream.is_open());

		class haplotypeAlternative {
		protected:

			std::vector<std::string> h1;
			std::vector<std::string> h2;
			double LL;
			std::map<std::string, std::pair<double, double> > ll_per_read;
			std::map<std::string, double> averaged_ll_per_read;

			int ll_computed_until_position;

		public:
			haplotypeAlternative()
			{
				LL = 0;
				ll_computed_until_position = -1;
			}

			std::pair<std::string, std::string> getHaplotypeAlleles(unsigned int pI)
			{
				return make_pair(h1.at(pI), h2.at(pI));
			}

			double getLL()
			{
				return LL;
			}

			int getLL_computedUntil()
			{
				return ll_computed_until_position;
			}

			void updateLikelihood(const std::set<std::string>& alleles_from_h1, const std::set<std::string>& alleles_from_h2, const std::vector<oneExonPosition>& pileUpPerPosition)
			{
				assert(h1.size() == h2.size());
				assert((int)h1.size() == (ll_computed_until_position + 1));

				const std::string& h1_underlying_position = h1.back();
				const std::string& h2_underlying_position = h2.back();

				double log_existingAllele = log(0.9999);
				double log_newAllele = log((1 - 0.9999) * 0.33); // on average three possible alleles

				LL += (alleles_from_h1.count(h1_underlying_position)) ? log_existingAllele : log_newAllele;
				LL += (alleles_from_h2.count(h2_underlying_position)) ? log_existingAllele : log_newAllele;

				std::set<std::string> read_IDs_modified;
				for(unsigned int rI = 0; rI < pileUpPerPosition.size(); rI++)
				{
					const oneExonPosition& r = pileUpPerPosition.at(rI);
					const std::string& readID = r.thisRead_ID;

					double position_likelihood_h1 = read_likelihood_per_position(h1_underlying_position, r.genotype, r.qualities, r.graphLevel);
					double position_likelihood_h2 = read_likelihood_per_position(h2_underlying_position, r.genotype, r.qualities, r.graphLevel);

					if(ll_per_read.count(readID) == 0)
					{
						ll_per_read[readID].first = 0;
						ll_per_read[readID].second = 0;
					}

					ll_per_read.at(readID).first += position_likelihood_h1;
					ll_per_read.at(readID).second += position_likelihood_h2;

					read_IDs_modified.insert(readID);
				}

				for(std::set<std::string>::iterator readIDit = read_IDs_modified.begin(); readIDit != read_IDs_modified.end(); readIDit++)
				{
					if(averaged_ll_per_read.count(*readIDit))
					{
						LL -= averaged_ll_per_read.at(*readIDit);
					}
					else
					{
						averaged_ll_per_read[*readIDit] = 0;
					}

					double new_average_p = 0.5 * exp(ll_per_read.at(*readIDit).first) + 0.5 * exp(ll_per_read.at(*readIDit).second);
					assert(new_average_p >= 0);
					assert(new_average_p <= 1);

					double new_average_logP = log(new_average_p);

					LL += new_average_logP;
					averaged_ll_per_read.at(*readIDit) = new_average_logP;
				}

				ll_computed_until_position++;
			}

			void extendHaplotypes(const std::pair<std::string, std::string>& extension)
			{
				h1.push_back(extension.first);
				h2.push_back(extension.second);
			}
		};

		class haplotypeAlternatives {
		public:
			std::list<haplotypeAlternative> runningAlternatives;
			int completedLevel;

			haplotypeAlternatives()
			{
				completedLevel = -1;
			}

			haplotypeAlternative getBestAlternative()
			{
				unsigned int l_max = 0;
				double ll_max = 0;

				unsigned int aI = 0;
				for(std::list<haplotypeAlternative>::iterator alternativeIt = runningAlternatives.begin(); alternativeIt != runningAlternatives.end(); alternativeIt++)
				{
					if((alternativeIt == runningAlternatives.begin()) || (alternativeIt->getLL() > ll_max))
					{
						l_max = aI;
						ll_max = alternativeIt->getLL();
					}
					aI++;
				}

				haplotypeAlternative forReturn;
				aI = 0;
				for(std::list<haplotypeAlternative>::iterator alternativeIt = runningAlternatives.begin(); alternativeIt != runningAlternatives.end(); alternativeIt++)
				{
					if(aI == l_max)
					{
						forReturn = *alternativeIt;
						break;
					}
					aI++;
				}

				return forReturn;
			}

			void processNewAlternatives(const std::vector<std::pair<std::string, std::string>>& alternatives)
			{
				if(completedLevel == -1)
				{
					for(unsigned int aI = 0; aI < alternatives.size(); aI++)
					{
						haplotypeAlternative A;
						A.extendHaplotypes(alternatives.at(aI));
						runningAlternatives.push_back(A);
					}
				}
				else
				{
					if(alternatives.size() == 1)
					{
						for(std::list<haplotypeAlternative>::iterator alternativeIt = runningAlternatives.begin(); alternativeIt != runningAlternatives.end(); alternativeIt++)
						{
							alternativeIt->extendHaplotypes(alternatives.front());
						}
					}
					else
					{
						std::list<haplotypeAlternative> runningAlternatives_copy = runningAlternatives;
						for(unsigned int aI = 0; aI < alternatives.size(); aI++)
						{
							if(aI == 0)
							{
								for(std::list<haplotypeAlternative>::iterator alternativeIt = runningAlternatives.begin(); alternativeIt != runningAlternatives.end(); alternativeIt++)
								{
									alternativeIt->extendHaplotypes(alternatives.front());
								}
							}
							else
							{
								for(std::list<haplotypeAlternative>::iterator alternativeIt = runningAlternatives_copy.begin(); alternativeIt != runningAlternatives_copy.end(); alternativeIt++)
								{
									haplotypeAlternative newAlternative = *alternativeIt;
									newAlternative.extendHaplotypes(alternatives.at(aI));
									runningAlternatives.push_back(newAlternative);
								}
							}
						}
					}
				}

				completedLevel++;
			}

			void updateLikelihood(std::set<std::string>& alleles_from_h1, std::set<std::string>& alleles_from_h2, std::vector<oneExonPosition>& pileUpPerPosition)
			{
				for(std::list<haplotypeAlternative>::iterator alternativeIt = runningAlternatives.begin(); alternativeIt != runningAlternatives.end(); alternativeIt++)
				{
					alternativeIt->updateLikelihood(alleles_from_h1, alleles_from_h2, pileUpPerPosition);
				}
			}

			std::map<std::string, double> getBackwardConfidence(unsigned int pI_start)
			{
				double max_LL = -1;
				int pI_stop = -1;

				for(std::list<haplotypeAlternative>::iterator alternativeIt = runningAlternatives.begin(); alternativeIt != runningAlternatives.end(); alternativeIt++)
				{
					int LL_available_to = alternativeIt->getLL_computedUntil();
					assert((int)LL_available_to >= (int)pI_start);
					if(alternativeIt == runningAlternatives.begin())
					{
						pI_stop = LL_available_to;
					}
					else
					{
						assert(pI_stop == LL_available_to);
					}

					if((alternativeIt == runningAlternatives.begin()) || (alternativeIt->getLL() > max_LL))
					{
						max_LL = alternativeIt->getLL();
					}
				}

				std::vector<double> relative_P;
				relative_P.resize(runningAlternatives.size());
				unsigned int aI = 0;
				double relative_P_sum = 0;
				for(std::list<haplotypeAlternative>::iterator alternativeIt = runningAlternatives.begin(); alternativeIt != runningAlternatives.end(); alternativeIt++)
				{
					relative_P.at(aI) = exp(alternativeIt->getLL() - max_LL);
					relative_P_sum += relative_P.back();
					aI++;
				}

				for(unsigned int i = 0; i < relative_P.size(); i++)
				{
					relative_P.at(i) = relative_P.at(i) / relative_P_sum;
				}

				std::map<std::string, double> forReturn;
				aI = 0;
				for(std::list<haplotypeAlternative>::iterator alternativeIt = runningAlternatives.begin(); alternativeIt != runningAlternatives.end(); alternativeIt++)
				{
					std::vector<std::string> h1;
					std::vector<std::string> h2;

					assert((int)pI_start <= pI_stop);
					for(int pI = pI_start; pI <= pI_stop; pI++)
					{
						std::pair<std::string, std::string> hA = alternativeIt->getHaplotypeAlleles(pI);
						h1.push_back(hA.first);
						h2.push_back(hA.second);
					}

					std::string h1_str = Utilities::join(h1, ";");
					std::string h2_str = Utilities::join(h2, ";");

					std::string h_comd = h1_str + "|" + h2_str;

					if(forReturn.count(h_comd))
					{
						forReturn[h_comd] = 0;
					}

					forReturn.at(h_comd) += relative_P.at(aI);

					aI++;
				}

				return forReturn;
			}

			void pruneAlternatives()
			{
				std::vector<std::pair<unsigned int, double> > likelihood_per_index;
				unsigned int aI = 0;
				for(std::list<haplotypeAlternative>::iterator alternativeIt = runningAlternatives.begin(); alternativeIt != runningAlternatives.end(); alternativeIt++)
				{
					likelihood_per_index.push_back(make_pair(aI, alternativeIt->getLL()));
					aI++;
				}

				std::sort(likelihood_per_index.begin(), likelihood_per_index.end(), [](std::pair<unsigned int, double> a, std::pair<unsigned int, double> b){return (a.second < b.second);});
				if(likelihood_per_index.size() > 1)
				{
					assert(likelihood_per_index.at(likelihood_per_index.size() - 1).second >= likelihood_per_index.at(likelihood_per_index.size() - 2).second);
				}

				double ll_max;
				double ll_min;
				double ll_cutoff = 1;

				std::vector<unsigned int> alternatives_sortedPosition;
				alternatives_sortedPosition.resize(runningAlternatives.size());
				for(unsigned int i = 0; i < likelihood_per_index.size(); i++)
				{
					int invertedI = likelihood_per_index.size() - i;
					alternatives_sortedPosition.at(likelihood_per_index.at(i).first) = invertedI;

					if(i == 0)
					{
						ll_min = likelihood_per_index.at(i).second;
					}
					if(i == (likelihood_per_index.size() - 1))
					{
						ll_max = likelihood_per_index.at(i).second;
					}
					if((ll_cutoff == 1) && (i >= ((double)likelihood_per_index.size() * (2.0/3.0))))
					{
						ll_cutoff = likelihood_per_index.at(i).second;
					}
				}

				assert(ll_max >= ll_cutoff);
				assert(ll_max >= ll_min);
				assert(ll_cutoff >= ll_min);

				aI = 0;
				std::list<haplotypeAlternative>::iterator alternativeIt = runningAlternatives.begin();
				while(alternativeIt != runningAlternatives.end())
				{
					std::list<haplotypeAlternative>::iterator thisIt = alternativeIt;
					alternativeIt++;
					if(alternatives_sortedPosition.at(aI) < (((double)likelihood_per_index.size() * (2.0/3.0))))
					{
						runningAlternatives.erase(thisIt);
					}
					aI++;
				}

				std::cout << "Likelihood pruning: remove 2/3 between min = " << ll_min << " and " << ll_max << " with cutoff " << ll_cutoff << "\n" << std::flush;
			}
		};

		haplotypeAlternatives runningHaplotypes;

		std::vector<unsigned int> combined_sequences_confidenceIndex;
		std::vector<std::pair<unsigned int, unsigned int> > confidenceIntervals_boundaries;
		std::vector<std::map<std::string, double> > pruning_confidences;

		unsigned int currentConfidenceInterval_start = 0;
		unsigned int currentConfidenceInterval_index = 0;

		for(unsigned int pI = 0; pI < combined_sequences_graphLevels.size(); pI++)
		{
			std::set<std::string> alleles_from_h1;
			std::set<std::string> alleles_from_h2;
			std::set<std::string> alleles_from_reads;

			// compute sets of plausible alleles

			for(std::set<std::string>::iterator haplotypeIDit = starting_haplotypes_1_set.begin(); haplotypeIDit != starting_haplotypes_1_set.end(); haplotypeIDit++)
			{
				std::string haplotypeID = *haplotypeIDit;
				std::string allele = combined_sequences.at(haplotypeID).substr(pI, 1);
				alleles_from_h1.insert(allele);
			}
			for(std::set<std::string>::iterator haplotypeIDit = starting_haplotypes_2_set.begin(); haplotypeIDit != starting_haplotypes_2_set.end(); haplotypeIDit++)
			{
				std::string haplotypeID = *haplotypeIDit;
				std::string allele = combined_sequences.at(haplotypeID).substr(pI, 1);
				alleles_from_h2.insert(allele);
			}
			for(unsigned int pileUpI = 0; pileUpI < pileUpPerPosition.at(pI).size(); pileUpI++)
			{
				oneExonPosition& onePositionSpecifier = pileUpPerPosition.at(pI).at(pileUpI);
				alleles_from_reads.insert(onePositionSpecifier.genotype);
			}
			std::vector<std::string> alleles_from_reads_vec(alleles_from_reads.begin(), alleles_from_reads.end());

			std::vector<std::pair<std::string, std::string>> possible_haplotype_extensions;

			for(unsigned int a1 = 0; a1 < alleles_from_reads.size(); a1++)
			{
				for(unsigned int a2 = 0; a2 < alleles_from_reads.size(); a2++)
				{
					std::pair<std::string, std::string> thisCombination = make_pair(alleles_from_reads_vec.at(a1), alleles_from_reads_vec.at(a2));
					possible_haplotype_extensions.push_back(thisCombination);
				}
			}

			runningHaplotypes.processNewAlternatives(possible_haplotype_extensions);
			runningHaplotypes.updateLikelihood(alleles_from_h1, alleles_from_h2, pileUpPerPosition.at(pI));

			std::cout << "\t\tPosition " << pI << " / " << combined_sequences_graphLevels.size() << ": " << runningHaplotypes.runningAlternatives.size() << " alternatives.\n";

			combined_sequences_confidenceIndex.push_back(currentConfidenceInterval_index);

			bool pruneHere = true;
			if(pruneHere)
			{
				unsigned int currentConfidenceInterval_stop = pI;
				confidenceIntervals_boundaries.push_back(make_pair(currentConfidenceInterval_start, currentConfidenceInterval_stop));

				std::map<std::string, double> localConfidences = runningHaplotypes.getBackwardConfidence(currentConfidenceInterval_start);
				pruning_confidences.push_back(localConfidences);


				runningHaplotypes.pruneAlternatives();

				std::cout << "\t\t\t post-pruning: " << runningHaplotypes.runningAlternatives.size() << "\n";

				currentConfidenceInterval_index++;
				currentConfidenceInterval_start = currentConfidenceInterval_stop + 1;
			}
		}

		if(currentConfidenceInterval_start != combined_sequences_graphLevels.size())
		{
			unsigned int currentConfidenceInterval_stop = (combined_sequences_graphLevels.size() - 1);
			confidenceIntervals_boundaries.push_back(make_pair(currentConfidenceInterval_start, currentConfidenceInterval_stop));

			std::map<std::string, double> localConfidences = runningHaplotypes.getBackwardConfidence(currentConfidenceInterval_start);
			pruning_confidences.push_back(localConfidences);

			currentConfidenceInterval_index++;
		}

		assert(currentConfidenceInterval_index >= 1);
		assert(combined_sequences_confidenceIndex.size() == combined_sequences_graphLevels.size());
		assert(confidenceIntervals_boundaries.size() == (combined_sequences_confidenceIndex.back() - 1));
		assert(confidenceIntervals_boundaries.size() == pruning_confidences.size());


		std::string outputFN_bestHaplotypeGuess = outputDirectory + "/R2_haplotypes_bestguess_"+locus+".txt";
		std::ofstream haplotypeBestGuess_outputStream;
		haplotypeBestGuess_outputStream.open(outputFN_bestHaplotypeGuess.c_str());
		assert(haplotypeBestGuess_outputStream.is_open());

		haplotypeAlternative bestHaplotype = runningHaplotypes.getBestAlternative();

		for(unsigned int pI = 0; pI < combined_sequences_graphLevels.size(); pI++)
		{
			std::pair<std::string, std::string> alleles = bestHaplotype.getHaplotypeAlleles(pI);

			std::vector<std::string> fieldsForLine;

			fieldsForLine.push_back(Utilities::ItoStr(pI));
			fieldsForLine.push_back(Utilities::ItoStr(combined_sequences_graphLevels.at(pI)));
			fieldsForLine.push_back(combined_sequences_graphLevels_individualType.at(pI));
			fieldsForLine.push_back(Utilities::ItoStr(combined_sequences_graphLevels_individualTypeNumber.at(pI)));
			fieldsForLine.push_back(Utilities::ItoStr(combined_sequences_graphLevels_individualPosition.at(pI)));

			fieldsForLine.push_back(alleles.first);
			fieldsForLine.push_back(alleles.second);

			unsigned int confidenceIndex = combined_sequences_confidenceIndex.at(pI);
			fieldsForLine.push_back(Utilities::ItoStr(confidenceIndex));

			std::vector<std::string> h1_forConfidence;
			std::vector<std::string> h2_forConfidence;

			for(unsigned int pI = confidenceIntervals_boundaries.at(confidenceIndex).first; pI <= confidenceIntervals_boundaries.at(confidenceIndex).second; pI++)
			{
				std::pair<std::string, std::string> hA = bestHaplotype.getHaplotypeAlleles(pI);
				h1_forConfidence.push_back(hA.first);
				h2_forConfidence.push_back(hA.second);
			}

			std::string h1_forConfidence_str = Utilities::join(h1_forConfidence, ";");
			std::string h2_forConfidence_str = Utilities::join(h2_forConfidence, ";");

			std::string h_forConfidence_combd = h1_forConfidence_str + "|" + h2_forConfidence_str;

			fieldsForLine.push_back(h1_forConfidence_str);
			fieldsForLine.push_back(h2_forConfidence_str);

			fieldsForLine.push_back(Utilities::DtoStr(pruning_confidences.at(confidenceIndex).at(h_forConfidence_combd)));

			haplotypeBestGuess_outputStream << Utilities::join(fieldsForLine, "\t") << "\n";
		}

		haplotypeBestGuess_outputStream.close();

	}
}

void HLATypeInference(std::string alignedReads_file, std::string graphDir, std::string sampleName)
{
	std::string graph = graphDir + "/graph.txt";
	assert(Utilities::fileReadable(graph));

	// define loci
	// std::vector<std::string> loci = {"A", "B", "C", "DQA1", "DQB1", "DRB1"};
	std::vector<std::string> loci = {"A"};

	// define locus -> exon
	std::map<std::string, std::vector<std::string> > loci_2_exons;

	std::vector<std::string> exons_A = {"exon_2", "exon_3"};
	loci_2_exons["A"] = exons_A;

	std::vector<std::string> exons_B = {"exon_2", "exon_3"};
	loci_2_exons["B"] = exons_B;

	std::vector<std::string> exons_C = {"exon_2", "exon_3"};
	loci_2_exons["C"] = exons_C;

	std::vector<std::string> exons_DQA1 = {"exon_2"};
	loci_2_exons["DQA1"] = exons_DQA1;

	std::vector<std::string> exons_DQB1 = {"exon_2"};
	loci_2_exons["DQB1"] = exons_DQB1;

	std::vector<std::string> exons_DRB1 = {"exon_2"};
	loci_2_exons["DRB1"] = exons_DRB1;


	// function to find right exon file
	std::vector<std::string> files_in_graphDir = filesInDirectory(graphDir);
	auto find_file_for_exon = [&](std::string locus, std::string exon) -> std::string
	{
		std::string forReturn;
		for(unsigned int fI = 0; fI < files_in_graphDir.size(); fI++)
		{
			std::vector<std::string> split_by_underscore = Utilities::split(files_in_graphDir.at(fI), "_");
			// if(!(split_by_underscore.size() >= 3))
			// {
				// std::cerr << "Can't split according to underscores: " << files_in_graphDir.at(fI) << "\n" << std::flush;
			// }
			// assert(split_by_underscore.size() >= 3);

			if(split_by_underscore.size() >= 4)
			{
				if(split_by_underscore.at(split_by_underscore.size()-4).substr(split_by_underscore.at(split_by_underscore.size()-4).length() - (locus.length()+1)) == ("/"+locus))
				{
					if((split_by_underscore.at(split_by_underscore.size()-2)+"_"+split_by_underscore.at(split_by_underscore.size()-1)) == (exon + ".txt"))
					{
						forReturn = files_in_graphDir.at(fI);
					}
				}
			}
		}
		if(! forReturn.length())
		{
			std::cerr << "find_file_for_exon -- problem -- " << locus << " -- " << exon << "\n";
			std::cerr << Utilities::join(files_in_graphDir, " ") << "\n" << std::flush;
			assert(forReturn.length());
		}
		return forReturn;
	};

	// translate location IDs to graph levels
	std::vector<std::string> graphLoci = readGraphLoci(graphDir);
	std::map<std::string, unsigned int> graphLocus_2_levels;
	for(unsigned int i = 0; i < graphLoci.size(); i++)
	{
		std::string locusID = graphLoci.at(i);
		assert(graphLocus_2_levels.count(locusID) == 0);
		graphLocus_2_levels[locusID] = i;
	}

	// load reads
	std::cout << Utilities::timestamp() << "HLATypeInference(..): Load reads.\n" << std::flush;
	std::vector<std::pair<seedAndExtend_return_local, seedAndExtend_return_local>> alignments;
	std::vector<oneReadPair> alignments_originalReads;

	double insertSize_mean;
	double insertSize_sd;

	read_shortReadAlignments_fromFile(alignedReads_file, alignments, alignments_originalReads, insertSize_mean, insertSize_sd);
	assert(alignments.size() == alignments_originalReads.size());

	std::cout << Utilities::timestamp() << "HLATypeInference(..): Load reads -- done. Have " << alignments.size() << " read pairs, IS mean " << insertSize_mean << " / sd " << insertSize_sd << ".\n" << std::flush;

	// read alignment statistics

	int alignmentStats_strandsValid = 0;
	int alignments_perfect = 0;
	int alignments_oneReadPerfect = 0;
	int alignmentStats_strandsValid_and_distanceOK = 0;
	std::vector<double> alignmentStats_strandsValid_distances;
	double alignmentStats_fractionOK_sum = 0;
	for(unsigned int alignmentI = 0; alignmentI < alignments.size(); alignmentI++)
	{

		std::pair<seedAndExtend_return_local, seedAndExtend_return_local>& alignedReadPair = alignments.at(alignmentI);

		if(alignedReadPair_strandsValid(alignedReadPair))
		{
			alignmentStats_strandsValid++;
			double pairsDistance = alignedReadPair_pairsDistanceInGraphLevels(alignedReadPair);
			alignmentStats_strandsValid_distances.push_back(pairsDistance);
			if(abs(pairsDistance - insertSize_mean) <= (5 * insertSize_sd))
			{
				alignmentStats_strandsValid_and_distanceOK++;
			}
		}

		double fractionOK_1 = alignmentFractionOK(alignedReadPair.first);
		double fractionOK_2 = alignmentFractionOK(alignedReadPair.second);

		if(fractionOK_1 == 1)
		{
			alignments_perfect++;
		}
		if(fractionOK_2 == 1)
		{
			alignments_perfect++;
		}
		if((fractionOK_1 == 1) || (fractionOK_2 == 1))
		{
			alignments_oneReadPerfect++;
		}

		alignmentStats_fractionOK_sum += fractionOK_1;
		alignmentStats_fractionOK_sum += fractionOK_2;
	}

	std::pair<double, double> alignmentStats_distance_meanMedian = meanMedian(alignmentStats_strandsValid_distances);
	double alignmentStats_fractionOK_avg = (alignments.size() > 0) ? (alignmentStats_fractionOK_sum / (2.0* (double)alignments.size())) : 0;

	if(! Utilities::directoryExists("../tmp/hla"))
	{
		Utilities::makeDir("../tmp/hla");
	}

	std::string outputDirectory = "../tmp/hla/"+sampleName;
	if(! Utilities::directoryExists(outputDirectory))
	{
		Utilities::makeDir(outputDirectory);
	}

	std::ofstream summaryStatisticsStream;
	std::string summaryStatisticsFilename = outputDirectory + "/" + "summaryStatistics.txt";
	summaryStatisticsStream.open(summaryStatisticsFilename.c_str());
	assert(summaryStatisticsStream.is_open());
	summaryStatisticsStream << "\nRead alignment statistics:\n";
	summaryStatisticsStream << "\t - Total number (paired) alignments:                 " << alignments.size() << "\n";
	summaryStatisticsStream << "\t - Alignment pairs with strands OK:                  " << alignmentStats_strandsValid << " (" << printPerc(alignmentStats_strandsValid, alignments.size()) << "%)\n";
	summaryStatisticsStream << "\t - Alignment pairs with strands OK && distance OK:   " << alignmentStats_strandsValid_and_distanceOK << " (" << printPerc(alignmentStats_strandsValid_and_distanceOK, alignments.size()) << "%)\n";
	summaryStatisticsStream << "\t - Alignment pairs with strands OK, mean distance:   " << alignmentStats_distance_meanMedian.first << "\n";
	summaryStatisticsStream << "\t - Alignment pairs with strands OK, median distance: " << alignmentStats_distance_meanMedian.second << "\n";
	summaryStatisticsStream << "\t - Alignment pairs, average fraction alignment OK:   " << alignmentStats_fractionOK_avg << "\n";
	summaryStatisticsStream << "\t - Alignment pairs, at least one alignment perfect:   " << alignments_oneReadPerfect << "\n";
	summaryStatisticsStream << "\t - Single alignments, perfect (total):   " << alignments_perfect << " (" << alignments.size()*2 << ")\n";

	summaryStatisticsStream.close();

	std::string outputFN_bestGuess = outputDirectory + "/R1_bestguess.txt";
	std::ofstream bestGuess_outputStream;
	bestGuess_outputStream.open(outputFN_bestGuess.c_str());
	assert(bestGuess_outputStream.is_open());
	bestGuess_outputStream << "Locus" << "\t" << "Chromosome" << "\t" << "Allele" << "\t" << "Q1" << "\t" << "Q2" << "\n";
	for(unsigned int locusI = 0; locusI < loci.size(); locusI++)
	{
		std::string locus = loci.at(locusI);

		std::string outputFN_allPairs = outputDirectory + "/R1_PP_"+locus+"_pairs.txt";

		std::cout << Utilities::timestamp() << "HLATypeInference(..): Making inference for " << locus << "\n" << std::flush;

		std::vector<int> combined_exon_sequences_graphLevels;
		std::vector<int> combined_exon_sequences_graphLevels_individualExon;
		std::vector<int> combined_exon_sequences_graphLevels_individualExonPosition;

		std::vector<std::string> combined_exon_sequences_locusIDs;
		std::map<std::string, std::string> combined_exon_sequences;

		for(unsigned int exonI = 0; exonI < loci_2_exons.at(locus).size(); exonI++)
		{
			std::string exonID = loci_2_exons.at(locus).at(exonI);
			std::cout << Utilities::timestamp() << "\tLocus" << locus << ", exon " << exonID << "\n" << std::flush;

			std::string exonFile = find_file_for_exon(locus, exonID);
			if(! Utilities::fileReadable(exonFile))
			{
				std::cerr << "HLATypeInference(..): Locus " << locus << ", exon " << exonID << ": Can't read file " << exonFile << "\n";
			}
			assert(Utilities::fileReadable(exonFile));

			std::ifstream exonInputStream;
			exonInputStream.open(exonFile.c_str());
			assert(exonInputStream.is_open());
			std::vector<std::string> exon_lines;
			while(exonInputStream.good())
			{
				std::string line;
				std::getline(exonInputStream, line);
				Utilities::eraseNL(line);
				exon_lines.push_back(line);
			}
			exonInputStream.close();

			std::string firstLine = exon_lines.at(0);
			std::vector<std::string> firstLine_fields = Utilities::split(firstLine, " ");
			assert(firstLine_fields.at(0) == "IndividualID");

			std::vector<std::string> exon_level_names(firstLine_fields.begin() + 1, firstLine_fields.end());
			std::string first_graph_locusID = exon_level_names.front();
			std::string last_graph_locusID = exon_level_names.back();

			assert(graphLocus_2_levels.count(first_graph_locusID));
			assert(graphLocus_2_levels.count(last_graph_locusID));



			unsigned int first_graph_level = graphLocus_2_levels.at(first_graph_locusID);
			unsigned int last_graph_level = graphLocus_2_levels.at(last_graph_locusID);

			std::cout << Utilities::timestamp() << "\tLocus" << locus << ", exon " << exonID << ": from " << first_graph_locusID << " (" << first_graph_level << ") to " << last_graph_locusID << " (" << last_graph_level << ").\n" << std::flush;


			assert(last_graph_level > first_graph_level);
			unsigned int expected_allele_length = last_graph_level - first_graph_level + 1;
			if(!(exon_level_names.size() == expected_allele_length))
			{
				std::cerr << "For locus " << locus << " exon " << exonID << " (" << exonFile << "), we have a problem with expected graph length.\n";
				std::cerr << "\t" << "expected_allele_length" << ": " << expected_allele_length << "\n";
				std::cerr << "\t" << "expected_allele_length" << ": " << expected_allele_length << "\n";
				std::cerr << std::flush;
			}
			assert(exon_level_names.size() == expected_allele_length);

			combined_exon_sequences_locusIDs.insert(combined_exon_sequences_locusIDs.end(), exon_level_names.begin(), exon_level_names.end());
			for(unsigned int lI = 0; lI < expected_allele_length; lI++)
			{
				unsigned int graphLevel = first_graph_level + lI;
				assert(graphLocus_2_levels.at(exon_level_names.at(lI)) == graphLevel);
				combined_exon_sequences_graphLevels.push_back(graphLevel);
				combined_exon_sequences_graphLevels_individualExon.push_back(exonI);
				combined_exon_sequences_graphLevels_individualExonPosition.push_back(lI);
			}


			for(unsigned int lI = 1; lI < exon_lines.size(); lI++)
			{
				if(exon_lines.at(lI).length())
				{
					std::vector<std::string> line_fields = Utilities::split(exon_lines.at(lI), " ");
					assert(line_fields.size() == firstLine_fields.size());
					std::string HLA_type = line_fields.at(0);
					std::vector<std::string> line_alleles(line_fields.begin()+1, line_fields.end());

					std::string HLA_type_sequence = Utilities::join(line_alleles, "");

					if(exonI == 0)
					{
						assert(combined_exon_sequences.count(HLA_type) == 0);
						combined_exon_sequences[HLA_type] = HLA_type_sequence;
					}
					else
					{
						assert(combined_exon_sequences.count(HLA_type));
						combined_exon_sequences.at(HLA_type) += HLA_type_sequence;
					}
				}
			}
		}

		std::map<int, unsigned int> graphLevel_2_exonPosition;
		std::map<int, unsigned int> graphLevel_2_exonPosition_individualExon;
		std::map<int, unsigned int> graphLevel_2_exonPosition_individualExonPosition;
		for(unsigned int pI = 0; pI < combined_exon_sequences_graphLevels.size(); pI++)
		{
			int graphLevel = combined_exon_sequences_graphLevels.at(pI);
			assert(graphLevel >= 0);
			graphLevel_2_exonPosition[graphLevel] = pI;
			graphLevel_2_exonPosition_individualExon[graphLevel] = combined_exon_sequences_graphLevels_individualExon.at(pI);
			graphLevel_2_exonPosition_individualExonPosition[graphLevel] = combined_exon_sequences_graphLevels_individualExonPosition.at(pI);

		}

		std::cout << Utilities::timestamp() << "Have collected " << combined_exon_sequences.size() << " sequences -- first level " << combined_exon_sequences_graphLevels.front() << ", last level " << combined_exon_sequences_graphLevels.back() << ".\n" << std::flush;

		std::map<std::string, unsigned int> HLAtype_2_clusterID;
		std::vector<std::set<std::string>> HLAtype_clusters;
		std::map<std::string, unsigned int> sequence_2_cluster;
		std::vector<std::string> cluster_2_sequence;

		for(std::map<std::string, std::string>::iterator HLAtypeIt = combined_exon_sequences.begin(); HLAtypeIt != combined_exon_sequences.end(); HLAtypeIt++)
		{
			std::string HLAtypeID = HLAtypeIt->first;
			std::string sequence = HLAtypeIt->second;
			if(sequence_2_cluster.count(sequence))
			{
				unsigned int cluster = sequence_2_cluster.at(sequence);
				HLAtype_clusters.at(cluster).insert(HLAtypeID);
				HLAtype_2_clusterID[HLAtypeID] = cluster;
			}
			else
			{
				std::set<std::string> newCluster;
				newCluster.insert(HLAtypeID);
				HLAtype_clusters.push_back(newCluster);
				unsigned int newClusterID = HLAtype_clusters.size() - 1;

				assert(sequence_2_cluster.count(sequence) == 0);
				sequence_2_cluster[sequence] = newClusterID;

				HLAtype_2_clusterID[HLAtypeID] = newClusterID;
			}
		}

		std::cout << Utilities::timestamp() << "Clustered into " << HLAtype_clusters.size() << " identical (over exons considered) clusters." << "\n" << std::flush;


		for(unsigned int clusterI = 0; clusterI < HLAtype_clusters.size(); clusterI++)
		{
			// std::cout << "\t\t\tcluster " << clusterI << "\n";
			for(std::set<std::string>::iterator typeIt = HLAtype_clusters.at(clusterI).begin(); typeIt != HLAtype_clusters.at(clusterI).end(); typeIt++)
			{
				std::string HLAtype = *typeIt;
				// std::cout << "\t\t\t\t" << HLAtype << "\n";
				assert(HLAtype_2_clusterID.at(HLAtype) == clusterI);

				if(typeIt == HLAtype_clusters.at(clusterI).begin())
				{
					std::string sequence = combined_exon_sequences.at(HLAtype);
					cluster_2_sequence.push_back(sequence);
				}
			}
		}

		// now transform reads into sequences specifying exon genotype value


		std::cout << Utilities::timestamp() << "Compute exon positions and specified genotypes from reads\n" << std::flush;

		std::vector< std::vector<oneExonPosition> > exonPositions_fromReads;

		auto oneReadAlignment_2_exonPositions = [&](seedAndExtend_return_local& alignment, oneRead& read, std::vector<oneExonPosition>& ret_exonPositions, seedAndExtend_return_local& paired_alignment, oneRead& paired_read) -> void {
			int alignment_firstLevel = alignment.alignment_firstLevel();
			int alignment_lastLevel = alignment.alignment_lastLevel();

			double thisRead_fractionOK = alignmentFractionOK(alignment);
			double pairedRead_fractionOK = alignmentFractionOK(paired_alignment);

			double thisRead_WeightedCharactersOK = alignmentWeightedOKFraction(read, alignment);
			double pairedRead_WeightedCharactersOK = alignmentWeightedOKFraction(paired_read, paired_alignment);

			std::pair<seedAndExtend_return_local, seedAndExtend_return_local> alignedReadPair = make_pair(alignment, paired_alignment);
			double pairs_strands_OK = alignedReadPair_strandsValid(alignedReadPair);
			double pairs_strands_distance = alignedReadPair_pairsDistanceInGraphLevels(alignedReadPair);

			// std::cout << "This alignment " << alignment_firstLevel << " - " << alignment_lastLevel << "\n";
			// std::cout << "\tvs combined exon " << combined_exon_sequences_graphLevels.front() << " - " << combined_exon_sequences_graphLevels.back() << "\n\n" << std::flush;

			if( ((alignment_firstLevel >= combined_exon_sequences_graphLevels.front()) && (alignment_firstLevel <= combined_exon_sequences_graphLevels.back())) ||
				((alignment_lastLevel >= combined_exon_sequences_graphLevels.front()) && (alignment_lastLevel <= combined_exon_sequences_graphLevels.back())) )
			{
				std::vector<oneExonPosition> readAlignment_exonPositions;

				int indexIntoOriginalReadData = -1;
				for(unsigned int cI = 0; cI < alignment.sequence_aligned.length(); cI++)
				{
					std::string sequenceCharacter = alignment.sequence_aligned.substr(cI, 1);
					std::string graphCharacter = alignment.graph_aligned.substr(cI, 1);
					int graphLevel = alignment.graph_aligned_levels.at(cI);

					if(graphLevel == -1)
					{
						// insertion relative to the graph - we need to extend last character

						assert(graphCharacter == "_");
						assert(sequenceCharacter != "_");

						indexIntoOriginalReadData++;
						int indexIntoOriginalReadData_correctlyAligned = indexIntoOriginalReadData;
						if(alignment.reverse)
						{
							indexIntoOriginalReadData_correctlyAligned = read.sequence.length() - indexIntoOriginalReadData_correctlyAligned - 1;
						}
						assert(indexIntoOriginalReadData_correctlyAligned >= 0);
						assert(indexIntoOriginalReadData_correctlyAligned <(int) read.sequence.length());

						std::string underlyingReadCharacter = read.sequence.substr(indexIntoOriginalReadData_correctlyAligned, 1);
						if(alignment.reverse)
						{
							underlyingReadCharacter = Utilities::seq_reverse_complement(underlyingReadCharacter);
						}
						assert(underlyingReadCharacter == sequenceCharacter);
						char qualityCharacter = read.quality.at(indexIntoOriginalReadData_correctlyAligned);

						if(readAlignment_exonPositions.size() > 0)
						{
							readAlignment_exonPositions.back().genotype.append(sequenceCharacter);
							readAlignment_exonPositions.back().alignment_edgelabels.append(graphCharacter);
							readAlignment_exonPositions.back().qualities.push_back(qualityCharacter);
							assert(readAlignment_exonPositions.back().genotype.length() == readAlignment_exonPositions.back().qualities.length());
							assert(readAlignment_exonPositions.back().alignment_edgelabels.length() == readAlignment_exonPositions.back().genotype.length());
						}
					}
					else
					{
						if(sequenceCharacter != "_")
						{
							indexIntoOriginalReadData++;
							int indexIntoOriginalReadData_correctlyAligned = indexIntoOriginalReadData;
							if(alignment.reverse)
							{
								indexIntoOriginalReadData_correctlyAligned = read.sequence.length() - indexIntoOriginalReadData_correctlyAligned - 1;
							}
							assert(indexIntoOriginalReadData_correctlyAligned >= 0);
							assert(indexIntoOriginalReadData_correctlyAligned < (int)read.sequence.length());

							std::string underlyingReadCharacter = read.sequence.substr(indexIntoOriginalReadData_correctlyAligned, 1);
							if(alignment.reverse)
							{
								underlyingReadCharacter = Utilities::seq_reverse_complement(underlyingReadCharacter);
							}
							assert(underlyingReadCharacter == sequenceCharacter);

							if(graphCharacter == "_")
							{

								assert(graphLevel != -1);

								char qualityCharacter = read.quality.at(indexIntoOriginalReadData_correctlyAligned);

								oneExonPosition thisPosition;
								thisPosition.graphLevel = graphLevel;
								thisPosition.genotype = sequenceCharacter;
								thisPosition.alignment_edgelabels = graphCharacter;
								thisPosition.qualities.push_back(qualityCharacter);

								thisPosition.thisRead_ID = read.name;
								thisPosition.pairedRead_ID = paired_read.name;
								thisPosition.thisRead_fractionOK = thisRead_fractionOK;
								thisPosition.pairedRead_fractionOK = pairedRead_fractionOK;
								thisPosition.pairs_strands_OK = pairs_strands_OK;
								thisPosition.pairs_strands_distance = pairs_strands_distance;
								thisPosition.thisRead_WeightedCharactersOK = thisRead_WeightedCharactersOK;
								thisPosition.pairedRead_WeightedCharactersOK = pairedRead_WeightedCharactersOK;

								readAlignment_exonPositions.push_back(thisPosition);

							}
							else
							{
								// two well-defined characters
								char qualityCharacter = read.quality.at(indexIntoOriginalReadData_correctlyAligned);

								oneExonPosition thisPosition;
								thisPosition.graphLevel = graphLevel;
								thisPosition.genotype = sequenceCharacter;
								thisPosition.alignment_edgelabels = graphCharacter;
								thisPosition.qualities.push_back(qualityCharacter);

								thisPosition.thisRead_ID = read.name;
								thisPosition.pairedRead_ID = paired_read.name;
								thisPosition.thisRead_fractionOK = thisRead_fractionOK;
								thisPosition.pairedRead_fractionOK = pairedRead_fractionOK;
								thisPosition.pairs_strands_OK = pairs_strands_OK;
								thisPosition.pairs_strands_distance = pairs_strands_distance;
								thisPosition.thisRead_WeightedCharactersOK = thisRead_WeightedCharactersOK;
								thisPosition.pairedRead_WeightedCharactersOK = pairedRead_WeightedCharactersOK;

								readAlignment_exonPositions.push_back(thisPosition);
							}

						}
						else
						{
							assert(sequenceCharacter == "_");
							if(graphCharacter == "_")
							{
								assert(graphLevel != -1);

								oneExonPosition thisPosition;
								thisPosition.graphLevel = graphLevel;
								thisPosition.genotype = "_";
								thisPosition.alignment_edgelabels = graphCharacter;
								thisPosition.qualities = "";

								thisPosition.thisRead_ID = read.name;
								thisPosition.pairedRead_ID = paired_read.name;
								thisPosition.thisRead_fractionOK = thisRead_fractionOK;
								thisPosition.pairedRead_fractionOK = pairedRead_fractionOK;
								thisPosition.pairs_strands_OK = pairs_strands_OK;
								thisPosition.pairs_strands_distance = pairs_strands_distance;
								thisPosition.thisRead_WeightedCharactersOK = thisRead_WeightedCharactersOK;
								thisPosition.pairedRead_WeightedCharactersOK = pairedRead_WeightedCharactersOK;

								readAlignment_exonPositions.push_back(thisPosition);
							}
							else
							{
								oneExonPosition thisPosition;
								thisPosition.graphLevel = graphLevel;
								thisPosition.genotype = "_";
								thisPosition.alignment_edgelabels = graphCharacter;
								thisPosition.qualities = "";

								thisPosition.thisRead_ID = read.name;
								thisPosition.pairedRead_ID = paired_read.name;
								thisPosition.thisRead_fractionOK = thisRead_fractionOK;
								thisPosition.pairedRead_fractionOK = pairedRead_fractionOK;
								thisPosition.pairs_strands_OK = pairs_strands_OK;
								thisPosition.pairs_strands_distance = pairs_strands_distance;
								thisPosition.thisRead_WeightedCharactersOK = thisRead_WeightedCharactersOK;
								thisPosition.pairedRead_WeightedCharactersOK = pairedRead_WeightedCharactersOK;

								readAlignment_exonPositions.push_back(thisPosition);
							}
						}
					}
				}

				int alongReadMode = 0;
				int lastPositionInExon = -1;
				for(unsigned int posInAlignment = 0; posInAlignment < readAlignment_exonPositions.size(); posInAlignment++)
				{
					oneExonPosition& thisPosition = readAlignment_exonPositions.at(posInAlignment);
					assert(thisPosition.graphLevel != -1);
					if(graphLevel_2_exonPosition.count(thisPosition.graphLevel))
					{
						assert((alongReadMode == 0) || (alongReadMode == 1));
						thisPosition.positionInExon = graphLevel_2_exonPosition.at(thisPosition.graphLevel);
						assert((lastPositionInExon == -1) || ((int)thisPosition.positionInExon == ((int)lastPositionInExon + 1)));
						lastPositionInExon = thisPosition.positionInExon;
						alongReadMode = 1;

						ret_exonPositions.push_back(thisPosition);
					}
					else
					{
						if(alongReadMode == 1)
						{
							alongReadMode = 2;
						}
					}
				}
			}

		};

		unsigned int readPairs_OK = 0;
		unsigned int readPairs_broken = 0;

		for(unsigned int readPairI = 0; readPairI < alignments.size(); readPairI++)
		{
			oneReadPair& originalReadPair = alignments_originalReads.at(readPairI);
			std::pair<seedAndExtend_return_local, seedAndExtend_return_local>& alignedReadPair = alignments.at(readPairI);

			std::vector<oneExonPosition> read1_exonPositions;
			std::vector<oneExonPosition> read2_exonPositions;
			oneReadAlignment_2_exonPositions(alignedReadPair.first, originalReadPair.reads.first, read1_exonPositions, alignedReadPair.second, originalReadPair.reads.second);
			oneReadAlignment_2_exonPositions(alignedReadPair.second, originalReadPair.reads.second, read2_exonPositions, alignedReadPair.first, originalReadPair.reads.first);

			// if(originalReadPair.reads.first.name == "@@A819GMABXX:8:2204:2901:85228#GATCAGAT/1")
			// {
				// std::cerr << "!!!!!!!!!!!!!!!!!" << "\n";
				// std::cerr << "Locus: " << locus << "\n";
				// std::cerr << "Read name: " << originalReadPair.reads.first.name << "\n";
				// std::cerr << "countMismatchesInExon(read1_exonPositions): " << countMismatchesInExon(read1_exonPositions) << "\n" << std::flush;
				// std::cerr << "Alignment:\n";
				// std::cerr << "\t" << alignedReadPair.first.graph_aligned << "\n";
				// std::cerr << "\t" << alignedReadPair.first.sequence_aligned << "\n";
				// std::cerr << "!!!!!!!!!!!!!!!!!" << "\n" << std::flush;
			// }

			if(
					alignedReadPair_strandsValid(alignedReadPair) &&
					(abs(alignedReadPair_pairsDistanceInGraphLevels(alignedReadPair) - insertSize_mean) <= (5 * insertSize_sd)) &&
					// (countMismatchesInExon(read1_exonPositions) < max_mismatches_perRead) &&
					// (countMismatchesInExon(read2_exonPositions) < max_mismatches_perRead)
					(alignmentFractionOK(alignedReadPair.first) >= min_alignmentFraction_OK) &&
					(alignmentFractionOK(alignedReadPair.second) >= min_alignmentFraction_OK) &&
					((alignmentWeightedOKFraction(originalReadPair.reads.first, alignedReadPair.first) >= min_oneRead_weightedCharactersOK) || (alignmentWeightedOKFraction(originalReadPair.reads.second, alignedReadPair.second) >= min_oneRead_weightedCharactersOK))
			)
			{
				// good

				// std::cout << "\t\t" << "readPair " << readPairI << ", pairing OK.\n" << std::flush;

				std::vector<oneExonPosition> thisRead_exonPositions = read1_exonPositions;
				thisRead_exonPositions.insert(thisRead_exonPositions.end(), read2_exonPositions.begin(), read2_exonPositions.end());
				if(thisRead_exonPositions.size() > 0)
					exonPositions_fromReads.push_back(thisRead_exonPositions);

				readPairs_OK++;
			}
			else
			{
				// bad

				// std::cout << "\t\t" << "readPair " << readPairI << "/" < < alignments.size() << ", pairing FAILED.\n" << std::flush;


				// if((read1_exonPositions.size() > 0) && (countMismatchesInExon(read1_exonPositions) < max_mismatches_perRead))
				if(0 && (read1_exonPositions.size() > 0) && (alignmentFractionOK(alignedReadPair.first) >= min_alignmentFraction_OK) && (alignmentFractionOK(alignedReadPair.second) >= min_alignmentFraction_OK))
					exonPositions_fromReads.push_back(read1_exonPositions);

				// if((read2_exonPositions.size() > 0) && (countMismatchesInExon(read2_exonPositions) < max_mismatches_perRead))
				if(0 && (read2_exonPositions.size() > 0) && (alignmentFractionOK(alignedReadPair.first) >= min_alignmentFraction_OK) && (alignmentFractionOK(alignedReadPair.second) >= min_alignmentFraction_OK))
					exonPositions_fromReads.push_back(read2_exonPositions);

				readPairs_broken++;
			}
		}

		std::cout << Utilities::timestamp() << "Mapped reads to exons. " << readPairs_OK << " pairs OK, " << readPairs_broken << " pairs broken." << "\n" << std::flush;

		// Pileup of mapped reads

		std::map<int, std::map<int, std::vector<oneExonPosition> > > pileUpPerPosition;
		for(unsigned int positionSpecifierI = 0; positionSpecifierI < exonPositions_fromReads.size(); positionSpecifierI++)
		{
			std::vector<oneExonPosition>& individualPositions = exonPositions_fromReads.at(positionSpecifierI);
			for(unsigned int positionI = 0; positionI < individualPositions.size(); positionI++)
			{
				oneExonPosition& onePositionSpecifier = individualPositions.at(positionI);

				int individualExon = graphLevel_2_exonPosition_individualExon.at(onePositionSpecifier.graphLevel);
				int individualExonPosition = graphLevel_2_exonPosition_individualExonPosition.at(onePositionSpecifier.graphLevel);

				pileUpPerPosition[individualExon][individualExonPosition].push_back(onePositionSpecifier);

			}
		}

		std::string fileName_pileUp = outputDirectory + "/R1_pileup_"+locus+".txt";
		std::ofstream pileUpStream;
		pileUpStream.open(fileName_pileUp.c_str());
		assert(pileUpStream.is_open());


		for(std::map<int, std::map<int, std::vector<oneExonPosition> > >::iterator exonIt = pileUpPerPosition.begin(); exonIt != pileUpPerPosition.end(); exonIt++)
		{
			int exon = exonIt->first;
			for(std::map<int, std::vector<oneExonPosition> >::iterator exonPosIt = pileUpPerPosition.at(exon).begin(); exonPosIt != pileUpPerPosition.at(exon).end(); exonPosIt++)
			{
				int exonPos = exonPosIt->first;
				std::vector<oneExonPosition> pileUp = exonPosIt->second;

				std::vector<std::string> fieldsPerLine;
				fieldsPerLine.push_back(Utilities::ItoStr(exon));
				fieldsPerLine.push_back(Utilities::ItoStr(exonPos));

				std::vector<std::string> piledUpGenotypes;

				for(unsigned int pI = 0; pI < pileUp.size(); pI++)
				{
					oneExonPosition piledPosition = pileUp.at(pI);

					std::vector<std::string> qualities_as_strings;
					for(unsigned int qI = 0; qI < piledPosition.qualities.size(); qI++)
					{
						char qC = piledPosition.qualities.at(qI);
						int qC_i = qC;
						qualities_as_strings.push_back(Utilities::ItoStr(qC_i));
					}

					std::string pileUpString = piledPosition.genotype
						+ " (" + Utilities::join(qualities_as_strings, ", ") + ")"
						+ " ["
						+ Utilities::DtoStr(piledPosition.thisRead_WeightedCharactersOK) + " "
						+ Utilities::DtoStr(piledPosition.pairedRead_WeightedCharactersOK) + " | "
						+ Utilities::DtoStr(piledPosition.thisRead_fractionOK) + " "
						+ Utilities::DtoStr(piledPosition.pairedRead_fractionOK) + " | "
						+ Utilities::ItoStr(piledPosition.pairs_strands_OK) + " "
						+ Utilities::DtoStr(piledPosition.pairs_strands_distance) + " | "
						+ piledPosition.thisRead_ID + " "
						+ piledPosition.pairedRead_ID
						+ "]";

					piledUpGenotypes.push_back(pileUpString);
				}

				fieldsPerLine.push_back(Utilities::join(piledUpGenotypes, ", "));


				pileUpStream << Utilities::join(fieldsPerLine, "\t") << "\n";
			}
		}
		pileUpStream.close();

		// likelihoods for reads

		std::cout << Utilities::timestamp() << "Compute likelihoods for all exon-overlapping reads (" << exonPositions_fromReads.size() << "), conditional on underlying exons.\n" << std::flush;
		std::vector<std::vector<double> > likelihoods_perCluster_perRead;
		std::vector<std::vector<int> > mismatches_perCluster_perRead;

		likelihoods_perCluster_perRead.resize(HLAtype_clusters.size());
		mismatches_perCluster_perRead.resize(HLAtype_clusters.size());


		// std::cout << "\n\n" << HLAtype_2_clusterID.at("A*30:73N") << " " << HLAtype_2_clusterID.at("A*29:02:08") << "\n" << std::flush;
		// assert( 1 == 0 );

		std::set<int> printClusters;
		// printClusters.insert(1789);
		// printClusters.insert(1646);


		for(unsigned int clusterI = 0; clusterI < HLAtype_clusters.size(); clusterI++)
		{
			std::vector<std::string> typesInCluster(HLAtype_clusters.at(clusterI).begin(), HLAtype_clusters.at(clusterI).end());
			std::string clusterName = Utilities::join(typesInCluster, "|");

			bool verbose = printClusters.count(clusterI);

			if(verbose)
			{
				std::cout << "CLUSTER " << clusterI << " " << clusterName << "\n";
			}

			std::string& clusterSequence = cluster_2_sequence.at(clusterI);

			for(unsigned int positionSpecifierI = 0; positionSpecifierI < exonPositions_fromReads.size(); positionSpecifierI++)
			{
				std::vector<oneExonPosition>& individualPositions = exonPositions_fromReads.at(positionSpecifierI);
				double log_likelihood = 0;
				int mismatches = 0;

				std::string readID;
				if(individualPositions.size() > 0)
				{
					readID = individualPositions.at(0).thisRead_ID;
				}

				// bool verbose = ((clusterI == 1127) && (readID == "@@B81EP5ABXX:8:2208:11879:23374#GATCAGAT/2") && 0);

				if(verbose)
					std::cout << "Likelihood calculation for read " << readID << " / cluster " << clusterI << "\n";



				for(unsigned int positionI = 0; positionI < individualPositions.size(); positionI++)
				{
					oneExonPosition& onePositionSpecifier = individualPositions.at(positionI);

					std::string exonGenotype = clusterSequence.substr(onePositionSpecifier.positionInExon, 1);
					std::string readGenotype = onePositionSpecifier.genotype;
					std::string readQualities = onePositionSpecifier.qualities;

					if(verbose)
					{
						std::cout << "\t" << positionI << " exon pos " << onePositionSpecifier.positionInExon << ": " << exonGenotype << " " << readGenotype << "\n" << std::flush;
					}

					assert(exonGenotype.length() == 1);
					assert(readGenotype.length() >= 1);
					unsigned int l_diff = readGenotype.length() - exonGenotype.length();
					if(exonGenotype == "_")
					{
						// assert(l_diff == 0);
						if(readGenotype == "_")
						{
							assert(onePositionSpecifier.graphLevel != -1);
							// likelihood 1 - intrinsic graph gap

							if(verbose)
							{
								std::cout << "\t\t" << "Intrinsic graph gap" << "\n";
							}

						}
						else
						{
							if(verbose)
							{
								std::cout << "\t\t" << "Insertion " << (1 + l_diff) << "\n";
							}

							log_likelihood += (log_likelihood_insertion * (1 + l_diff));
						}
					}
					else
					{

						// score from first position match
						if(readGenotype.substr(0, 1) == "_")
						{
							log_likelihood += log_likelihood_deletion;

							if(verbose)
							{
								std::cout << "\t\t" << "Deletion" << "\n";
							}
						}
						else
						{
							assert(readQualities.length());
							double pCorrect = Utilities::PhredToPCorrect(readQualities.at(0));
							assert((pCorrect > 0) && (pCorrect <= 1));

							if(!(pCorrect >= 0.25))
							{
								std::cerr << "pCorrect = " << pCorrect << "\n" << std::flush;
							}
							assert(pCorrect >= 0.25);

							if(exonGenotype == readGenotype.substr(0, 1))
							{
								if(verbose)
								{
									std::cout << "\t\t" << "Match " << pCorrect << "\n";
								}

								log_likelihood += log(pCorrect);
							}
							else
							{
								double pIncorrect = (1 - pCorrect)*(1.0/3.0);
								assert(pIncorrect <= 0.75);
								assert((pIncorrect > 0) && (pIncorrect < 1));
								log_likelihood += log(pIncorrect);

								if(verbose)
								{
									std::cout << "\t\t" << "Mismatch " << pIncorrect << "\n";
								}
							}
						}
						// if read allele is longer
						log_likelihood += (log_likelihood_insertion * l_diff);

						if(l_diff > 0)
						{
							if(verbose)
							{
								std::cout << "\t\t" << "Insertion " << l_diff << "\n";
							}
						}
					}

					if(readGenotype != "_")
					{
						if(readGenotype != exonGenotype)
						{
							mismatches++;
						}
					}

					if(verbose)
					{
						std::cout << "\t\t" << "Running log likelihood: " << log_likelihood << "\n";
					}
				}

				if(individualPositions.size() > 0)
				{
					if((clusterI == 1160) || (clusterI == 1127))
					{
						// std::cout << locus << " " << readID << " cluster " << clusterI << ": " << log_likelihood << "\n" << std::flush;
					}
				}

				// std::cout << "cluster " << clusterI << ", position sequence " << positionSpecifierI << ": " << log_likelihood << "\n";

				likelihoods_perCluster_perRead.at(clusterI).push_back(log_likelihood);
				mismatches_perCluster_perRead.at(clusterI).push_back(mismatches);

			}

			assert(likelihoods_perCluster_perRead.at(clusterI).size() == exonPositions_fromReads.size());
			assert(mismatches_perCluster_perRead.at(clusterI).size() == exonPositions_fromReads.size());
		}

		std::cout << Utilities::timestamp() << "Compute likelihoods for all exon cluster pairs (" << HLAtype_clusters.size() << "**2/2)\n" << std::flush;

		std::vector<double> LLs;
		std::vector<double> Mismatches_avg;
		std::vector<double> Mismatches_min;

		std::vector<std::pair<unsigned int, unsigned int> > LLs_clusterIs;
		std::vector<unsigned int> LLs_indices;

		for(unsigned int clusterI1 = 0; clusterI1 < HLAtype_clusters.size(); clusterI1++)
		{
			for(unsigned int clusterI2 = clusterI1; clusterI2 < HLAtype_clusters.size(); clusterI2++)
			{
				double pair_log_likelihood = 0;

				double mismatches_sum_averages = 0;
				double mismatches_sum_min = 0;

				for(unsigned int positionSpecifierI = 0; positionSpecifierI < exonPositions_fromReads.size(); positionSpecifierI++)
				{
					std::string readID;
					if(exonPositions_fromReads.at(positionSpecifierI).size() > 0)
					{
						readID = exonPositions_fromReads.at(positionSpecifierI).at(0).thisRead_ID;
					}


					double LL_thisPositionSpecifier_cluster1 = likelihoods_perCluster_perRead.at(clusterI1).at(positionSpecifierI);
					double LL_thisPositionSpecifier_cluster2 = likelihoods_perCluster_perRead.at(clusterI2).at(positionSpecifierI);

					// double LL_average = log( (1 + (exp(LL_thisPositionSpecifier_cluster2+log(0.5)))/exp(LL_thisPositionSpecifier_cluster1+log(0.5)) )) + (LL_thisPositionSpecifier_cluster1+log(0.5));

					double LL_average_2 = log(0.5 * exp(LL_thisPositionSpecifier_cluster1) + 0.5 * exp(LL_thisPositionSpecifier_cluster2));

					int mismatches_cluster1 = mismatches_perCluster_perRead.at(clusterI1).at(positionSpecifierI);
					int mismatches_cluster2 = mismatches_perCluster_perRead.at(clusterI2).at(positionSpecifierI);

					mismatches_sum_averages += ((double)(mismatches_cluster1 + mismatches_cluster2) / 2.0);
					mismatches_sum_min += ((mismatches_cluster1 < mismatches_cluster2) ? mismatches_cluster1 : mismatches_cluster2);

					pair_log_likelihood += LL_average_2;

					// if ( ((clusterI1 == 1160) && (clusterI2 == 1640)) || ((clusterI1 == 1127) && (clusterI2 == 1640)))
					// {
						// std::cout << "Cluster pair " << clusterI1 << " / " << clusterI2 << " read " << readID << ": " << LL_thisPositionSpecifier_cluster1 << " and " << LL_thisPositionSpecifier_cluster2 << ": " << LL_average_2 << "\n" << std::flush;
					// }
				}

				LLs.push_back(pair_log_likelihood);
				Mismatches_avg.push_back(mismatches_sum_averages);
				Mismatches_min.push_back(mismatches_sum_min);

				LLs_clusterIs.push_back(make_pair(clusterI1, clusterI2));

				LLs_indices.push_back(LLs.size() - 1);
			}
		}

		std::sort(LLs_indices.begin(), LLs_indices.end(), [&](unsigned int a, unsigned int b) {
			if(LLs.at(a) == LLs.at(b))
			{
				return (Mismatches_avg.at(b) < Mismatches_avg.at(a));
			}
			else
			{
				return (LLs.at(a) < LLs.at(b));
			}
		});

		std::reverse(LLs_indices.begin(), LLs_indices.end());


		std::pair<double, unsigned int> maxPairI = Utilities::findVectorMax(LLs);
		std::cout << Utilities::timestamp() << "Done. (One) maximum pair is " << maxPairI.second << " with LL = " << maxPairI.first << "\n" << std::flush;

		std::vector<double> LLs_normalized;
		double LL_max = maxPairI.first;
		double P_sum = 0;
		for(unsigned int cI = 0; cI < LLs_clusterIs.size(); cI++)
		{
			double LL = LLs.at(cI);
			double P = exp(LL - LL_max);
			P_sum += P;
		}
		for(unsigned int cI = 0; cI < LLs_clusterIs.size(); cI++)
		{
			double LL = LLs.at(cI);
			double P = exp(LL - LL_max);
			double P_normalized = P / P_sum;
			assert(P_normalized >= 0);
			assert(P_normalized <= 1);
			LLs_normalized.push_back(P_normalized);
		}

		std::ofstream allPairsStream;
		allPairsStream.open(outputFN_allPairs.c_str());
		assert(allPairsStream.is_open());
		allPairsStream << "ClusterID" << "\t" << "P" << "\t" << "LL" << "\t" << "Mismatches_avg" << "\n";

		std::vector<std::string> LLs_identifiers;
		std::map<int, double> clusterI_overAllPairs;
		for(unsigned int cII = 0; cII < LLs_indices.size(); cII++)
		{
			unsigned int cI = LLs_indices.at(cII);
			std::pair<unsigned int, unsigned int>& clusters = LLs_clusterIs.at(cI);

			std::vector<std::string> cluster1_members(HLAtype_clusters.at(clusters.first).begin(), HLAtype_clusters.at(clusters.first).end());
			std::vector<std::string> cluster2_members(HLAtype_clusters.at(clusters.second).begin(), HLAtype_clusters.at(clusters.second).end());

			std::string id = Utilities::join(cluster1_members, ";") + "/" + Utilities::join(cluster2_members, ";");

			LLs_identifiers.push_back(id);

			allPairsStream << id << "\t" << LLs_normalized.at(cI) << "\t" << LLs.at(cI) << "\t" << Mismatches_avg.at(cI) << "\n";

			if(clusterI_overAllPairs.count(clusters.first) == 0)
			{
				clusterI_overAllPairs[clusters.first] = 0;
			}
			clusterI_overAllPairs[clusters.first] += LLs_normalized.at(cI);

			if(clusters.second != clusters.first)
			{
				if(clusterI_overAllPairs.count(clusters.second) == 0)
				{
					clusterI_overAllPairs[clusters.second] = 0;
				}
				clusterI_overAllPairs[clusters.second] += LLs_normalized.at(cI);
			}
		}

		allPairsStream.close();

		std::pair<double, int> bestGuess_firstAllele = Utilities::findIntMapMax(clusterI_overAllPairs);
		std::string bestGuess_firstAllele_ID = Utilities::join(std::vector<std::string>(HLAtype_clusters.at(bestGuess_firstAllele.second).begin(), HLAtype_clusters.at(bestGuess_firstAllele.second).end()), ";");
		assert(bestGuess_firstAllele.first >= 0);
		assert(bestGuess_firstAllele.second >= 0);

		std::map<int, double> bestGuess_secondAllele_alternatives;
		std::map<int, double> bestGuess_secondAllele_alternatives_mismatches;

		for(unsigned int cI = 0; cI < LLs_clusterIs.size(); cI++)
		{
			std::pair<unsigned int, unsigned int>& clusters = LLs_clusterIs.at(cI);

			if((int)clusters.first == bestGuess_firstAllele.second)
			{
				assert(bestGuess_secondAllele_alternatives.count(clusters.second) == 0);
				bestGuess_secondAllele_alternatives[clusters.second] = LLs_normalized.at(cI);
				bestGuess_secondAllele_alternatives_mismatches[clusters.second] = Mismatches_min.at(cI);
			}
			else
			{
				if((int)clusters.second == bestGuess_firstAllele.second)
				{
					assert(bestGuess_secondAllele_alternatives.count(clusters.first) == 0);
					bestGuess_secondAllele_alternatives[clusters.first] = LLs_normalized.at(cI);
					bestGuess_secondAllele_alternatives_mismatches[clusters.first] = Mismatches_min.at(cI);
				}
			}
		}

		std::pair<double, int> oneBestGuess_secondAllele = Utilities::findIntMapMax(bestGuess_secondAllele_alternatives);
		assert(oneBestGuess_secondAllele.first >= 0);
		assert(oneBestGuess_secondAllele.first <= 1);

		std::map<int, double> mismatches_allBestGuessPairs;
		for(std::map<int, double>::iterator secondAlleleIt = bestGuess_secondAllele_alternatives.begin(); secondAlleleIt != bestGuess_secondAllele_alternatives.end(); secondAlleleIt++)
		{
			int cluster = secondAlleleIt->first;
			double LL_normalized = secondAlleleIt->second;
			if(LL_normalized == oneBestGuess_secondAllele.first)
			{
				mismatches_allBestGuessPairs[cluster] =  -1 * bestGuess_secondAllele_alternatives_mismatches.at(cluster);
			}
		}

		std::pair<double, int> bestGuess_secondAllele = Utilities::findIntMapMax(mismatches_allBestGuessPairs);

		std::string bestGuess_secondAllele_ID = Utilities::join(std::vector<std::string>(HLAtype_clusters.at(bestGuess_secondAllele.second).begin(), HLAtype_clusters.at(bestGuess_secondAllele.second).end()), ";");

		bestGuess_outputStream << locus << "\t" << 1 << "\t" << bestGuess_firstAllele_ID << "\t" << bestGuess_firstAllele.first << "\t" << bestGuess_secondAllele.first << "\n";
		bestGuess_outputStream << locus << "\t" << 2 << "\t" << bestGuess_secondAllele_ID << "\t" << oneBestGuess_secondAllele.first << "\t" << bestGuess_secondAllele.first << "\n";

		unsigned int maxPairPrint = (LLs_indices.size() > 10) ? 10 : LLs_indices.size();
		for(unsigned int LLi = 0; LLi < maxPairPrint; LLi++)
		{
			unsigned int pairIndex = LLs_indices.at(LLi);
			std::pair<unsigned int, unsigned int>& clusters = LLs_clusterIs.at(pairIndex);
			std::cout << "#" << (LLi+1) << ": " << clusters.first << " / " << clusters.second << ": " << LLs.at(pairIndex) << " absolute and " << exp(LLs.at(pairIndex) - maxPairI.first) << " relative." << "\n" << std::flush;

			std::vector<std::string> cluster1_members(HLAtype_clusters.at(clusters.first).begin(), HLAtype_clusters.at(clusters.first).end());
			std::vector<std::string> cluster2_members(HLAtype_clusters.at(clusters.second).begin(), HLAtype_clusters.at(clusters.second).end());

			std::cout << "\tcluster " << clusters.first << ": " << Utilities::join(cluster1_members, ", ") << "\n";
			std::cout << "\tcluster " << clusters.second << ": " << Utilities::join(cluster2_members, ", ") << "\n" << std::flush;

			std::cout << "\tMismatches " << Mismatches_avg.at(pairIndex) << " avg / " << Mismatches_min.at(pairIndex) << " min\n" << std::flush;

			// bool equal = (LLs.at(pairIndex) == LLs.at(LLs_indices.at(0)));
			// std::cout << "\tEqual: " << equal << "\n" << std::flush;


		}

		assert(LLs.at(LLs_indices.at(0)) == maxPairI.first);
		if(LLs_indices.size() > 1)
		{
			assert(LLs.at(LLs_indices.at(0)) >= LLs.at(LLs_indices.at(1)));
		}
	}

	bestGuess_outputStream.close();
}



