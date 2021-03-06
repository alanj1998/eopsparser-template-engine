#include <fstream>
#include "VirtualMachine.hpp"
#include "../../Generics/InstanceType.hpp"
#include <iostream>
#include "../../../Helpers/helperFunctions.hpp"
#include <vector>
#include "StorageProfile.hpp"
#include <regex>
#include <nlohmann/json.hpp>

using Json = nlohmann::json;

namespace EOPSTemplateEngine::Azure::Compute {
    VirtualMachine::VirtualMachine(std::string &name, std::string &resourceType): GenericAzureResource(name, resourceType) {
        this->storageProfile = new StorageProfile();
        this->hardwareProfile = new HardwareProfile();
    }

    Json VirtualMachine::ToJson() {
        Json j = GenericAzureResource::ToJson();
        Json vmProperties = Json::object();

        vmProperties["hardwareProfile"] = this->hardwareProfile->ToJson();
        vmProperties["storageProfile"] = this->storageProfile->ToJson();

        j["properties"] = vmProperties;
        return j;
    }

    void VirtualMachine::setInstanceType(std::string &os, float cpu, float ram, std::string &optimisation) {
        std::ifstream jsonFile;

        for (auto &c: os) {
            c = ::tolower(c);
        }

        if (os.find("windows") != std::string::npos) {
            jsonFile.open(AZURE_WINDOWS_INSTANCES);
        } else {
            jsonFile.open(AZURE_LINUX_INSTANCES);
        }

        auto *chosenInstance = new Generics::InstanceType();
        bool isFound = false;

        if (jsonFile.is_open()) {
            Json j = Json::parse(jsonFile);
            jsonFile.close(); // closing the file straight away

            std::vector<Generics::InstanceType> instanceTypes = j.at(optimisation);
            std::sort(instanceTypes.begin(), instanceTypes.end(), Generics::sortByCpuAndRam);

            for (struct Generics::InstanceType &instance : instanceTypes) {
                if (instance.isGPUEnabled) {
                    continue;
                }

                if (instance.cpu >= cpu && instance.ram >= ram) {
                    chosenInstance->cpu = instance.cpu;
                    chosenInstance->ram = instance.ram;
                    chosenInstance->name = instance.name;
                    isFound = true;
                    break;
                }
            }

            if (!isFound) {
                std::cout << "Did not find a match...";
                std::cout << "Using: " << instanceTypes[0].name << std::endl;
                chosenInstance = &instanceTypes[0];
            }
        } else {
            std::cout << "Error opening the json file. Creating Standard_B1ls instance."
                      << std::endl;
            auto *errorInstance = new Generics::InstanceType();
            errorInstance->name = "Standard_B1ls";
            chosenInstance = errorInstance;
        }

        this->hardwareProfile->setVMSize(chosenInstance->name);
    }

    std::vector<Generics::AZLocation> VirtualMachine::getAZs() {
        std::ifstream jsonFile;
        std::string type = this->getType();

        jsonFile.open(AZURE_AVAILABILITY_ZONES);

        if(jsonFile.is_open()) {
            std::vector<Generics::AZLocation> azLocations = Json::parse(jsonFile);
            jsonFile.close();

            for(auto it = azLocations.begin(); it != azLocations.end(); ++it){
                std::vector<std::string> v = it->availableResources;
                if(std::find(v.begin(), v.end(), type) == v.end()) {
                    it = azLocations.erase(it);
                    --it;
                } else {
                    if(this->getType() == "Microsoft.Compute/virtualMachines") {
                        std::string regexBuilder = "(";
                        for(const std::string &str: it->enabledVMSizes.value()) {
                            regexBuilder += str + '|';
                        }
                        regexBuilder.pop_back();
                        regexBuilder += ")";

                        std::regex r{regexBuilder};
                        if(!std::regex_match(this->hardwareProfile->getVMSize(), r)) {
                            it = azLocations.erase(it);
                            --it;
                        }
                    }
                }
            }

            return azLocations;
        }
    }

    void VirtualMachine::setFromParsedResource(EOPSNativeLib::Models::VirtualMachine *res) {
        std::string optimisation = res->Optimisation;

        if (!res->GPUs.empty()) {
            optimisation = "GPU";
        }

        this->setInstanceType(res->OS, res->Cores, res->Ram, optimisation);
        this->setLocation(res->Location);
        this->setOs(res->OS);
    }

    void VirtualMachine::setOs(std::string &os) {
        std::string operatingSystem = os;
        std::string version = "latest";
        std::string latest = "latest";

        if (os.find('@') != std::string::npos) {
            std::vector<std::string> split = Helpers::HelperFunctions::splitStringByDelimiter(os, '@');

            operatingSystem = split[0];
            version = split[1];
        }

        std::ifstream jsonFile;
        jsonFile.open(AZURE_OS_IMAGES);

        if (jsonFile.is_open()) {
            Json j = Json::parse(jsonFile);
            jsonFile.close();

            transform(operatingSystem.begin(), operatingSystem.end(), operatingSystem.begin(), ::toupper);

            if (operatingSystem.find("UBUNTU") != std::string::npos) {
                std::vector<ImageReference> s = j.at("UBUNTU");

                if (operatingSystem == "UBUNTU") {
                    operatingSystem = "UbuntuServer";
                }

                if (version == "latest") {
                    this->storageProfile->setImageReference(s[0]);
                } else {
                    std::vector<std::string> allSkus;
                    for (auto &l: s) {
                        allSkus.push_back(l.sku);
                    }
                    int index = Helpers::HelperFunctions::returnIndexOfClosestVersion(version, allSkus);
                    this->storageProfile->setImageReference(s[index]);
                }
            } else if (operatingSystem.find("CENTOS") != std::string::npos) {
                std::vector<ImageReference> s = j.at("CENTOS");
                ImageReference latestOs = s[0];

                for (auto it = s.begin(); it != s.end();) {
                    std::string offer = it->offer;
                    transform(offer.begin(), offer.end(), offer.begin(), ::toupper);

                    if (offer != operatingSystem) {
                        it = s.erase(it);
                    } else {
                        ++it;
                    }
                }

                if (s.empty()) {
                    std::cout << "Could not find your distribution. Setting to latest version of CentOS...";
                    this->storageProfile->setImageReference(latestOs);
                } else {
                    std::vector<std::string> allVersions;
                    for (auto &l: s) {
                        allVersions.push_back(l.version);
                    }
                    int index = Helpers::HelperFunctions::returnIndexOfClosestVersion(version,
                                                                                                     allVersions);
                    this->storageProfile->setImageReference(s[index]);
                }
            } else if (operatingSystem.find("COREOS") != std::string::npos) {
                std::vector<ImageReference> s = j.at("COREOS");
                std::vector<std::string> allVersions;
                for (auto &l: s) {
                    allVersions.push_back(l.version);
                }
                int index = Helpers::HelperFunctions::returnIndexOfClosestVersion(version, allVersions);
                this->storageProfile->setImageReference(s[index]);
            } else if (operatingSystem.find("DEBIAN") != std::string::npos) {
                std::vector<ImageReference> s = j.at("DEBIAN");

                this->storageProfile->setImageReference(s[0]); //only one version of debian
            } else if (operatingSystem.find("OPENSUSE") != std::string::npos) {
                std::vector<ImageReference> s = j.at("OPENSUSE");

                this->storageProfile->setImageReference(s[0]); //only one version of openSuse
            } else if (operatingSystem.find("RHEL") != std::string::npos) {
                std::vector<ImageReference> s = j.at("RHEL");
                ImageReference latestOs = s[0];
                if (version == "latest") {
                    this->storageProfile->setImageReference(s[0]);
                } else {
                    for (auto it = s.begin(); it != s.end();) {
                        std::string offer = it->offer;
                        transform(offer.begin(), offer.end(), offer.begin(), ::toupper);

                        if (offer != operatingSystem) {
                            it = s.erase(it);
                        } else {
                            ++it;
                        }
                    }

                    if (s.empty()) {
                        std::cout << "Could not find your distribution. Setting to latest version of RHEL...";
                        this->storageProfile->setImageReference(latestOs);
                    }

                    std::vector<std::string> allSkus;
                    for (auto &l: s) {
                        allSkus.push_back(l.sku);
                    }
                    int index = Helpers::HelperFunctions::returnIndexOfClosestVersion(version, allSkus);
                    this->storageProfile->setImageReference(s[index]);
                }
            } else if (operatingSystem.find("SLES") != std::string::npos) {
                std::vector<ImageReference> s = j.at("SLES");
                ImageReference latestOs = s[0];

                if (version == "latest") {
                    this->storageProfile->setImageReference(s[0]);
                } else {
                    for (auto it = s.begin(); it != s.end();) {
                        std::string offer = it->offer;
                        transform(offer.begin(), offer.end(), offer.begin(), ::toupper);

                        if (offer != operatingSystem) {
                            it = s.erase(it);
                        } else {
                            ++it;
                        }
                    }

                    if (s.empty()) {
                        std::cout << "Could not find your distribution. Setting to latest version of SLES...";
                        this->storageProfile->setImageReference(latestOs);
                    }

                    std::vector<std::string> allSkus;
                    for (auto &l: s) {
                        allSkus.push_back(l.sku);
                    }
                    int index = Helpers::HelperFunctions::returnIndexOfClosestVersion(version, allSkus);
                    this->storageProfile->setImageReference(s[index]);
                }
            } else if (operatingSystem.find("WINDOWS") != std::string::npos) {
                std::vector<ImageReference> s = j.at("WINDOWS");

                std::vector<std::string> allSkus;
                for (auto &l: s) {
                    allSkus.push_back(l.sku);
                }
                int index = Helpers::HelperFunctions::returnIndexOfClosestVersion(version, allSkus);
                this->storageProfile->setImageReference(s[index]);
            } else {
                std::vector<ImageReference> s = j.at("UBUNTU");

                if (operatingSystem == "UBUNTU") {
                    operatingSystem = "UbuntuServer";
                }

                std::vector<std::string> allSkus;
                for (auto &l: s) {
                    allSkus.push_back(l.sku);
                }
                int index = Helpers::HelperFunctions::returnIndexOfClosestVersion(latest, allSkus);
                this->storageProfile->setImageReference(s[index]);
            }
        } else {
            std::cout << "Failed to open the json file for OS! Setting it as ubuntu@latest" << std::endl;
            auto *ir = new ImageReference();
            ir->offer = "UbuntuServer";
            ir->publisher = "Canonical";
            ir->version = "latest";
            this->storageProfile->setImageReference(*ir);
        }
    }
}