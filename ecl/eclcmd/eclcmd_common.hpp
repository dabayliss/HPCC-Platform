/*##############################################################################

    Copyright (C) 2011 HPCC Systems.

    All rights reserved. This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
############################################################################## */

#ifndef ECLCMD_COMMON_HPP
#define ECLCMD_COMMON_HPP

//=========================================================================================

interface IEclCommand : extends IInterface
{
    virtual bool parseCommandLineOptions(ArgvIterator &iter)=0;
    virtual bool finalizeOptions(IProperties *globals)=0;
    virtual int processCMD()=0;
    virtual void usage()=0;
};

typedef IEclCommand *(*EclCommandFactory)(const char *cmdname);

#define ECLOPT_SERVER "--server"
#define ECLOPT_SERVER_INI "eclWatchIP"
#define ECLOPT_SERVER_ENV "ECL_WATCH_IP"
#define ECLOPT_SERVER_DEFAULT "."

#define ECLOPT_PORT "--port"
#define ECLOPT_PORT_INI "eclWatchPort"
#define ECLOPT_PORT_ENV "ECL_WATCH_PORT"
#define ECLOPT_PORT_DEFAULT "8010"

#define ECLOPT_USERNAME "--username"
#define ECLOPT_USERNAME_INI "eclUserName"
#define ECLOPT_USERNAME_ENV "ECL_USER_NAME"

#define ECLOPT_PASSWORD "--password"
#define ECLOPT_PASSWORD_INI "eclPassword"
#define ECLOPT_PASSWORD_ENV "ECL_PASSWORD"

#define ECLOPT_ACTIVATE "--activate"
#define ECLOPT_ACTIVATE_INI "activateDefault"
#define ECLOPT_ACTIVATE_ENV NULL

#define ECLOPT_WUID "--wuid"
#define ECLOPT_CLUSTER "--cluster"
#define ECLOPT_NAME "--name"
#define ECLOPT_ACTIVATE "--activate"
#define ECLOPT_QUERYSET "--queryset"
#define ECLOPT_VERSION "--version"

bool extractEclCmdOption(StringBuffer & option, IProperties * globals, const char * envName, const char * propertyName, const char * defaultPrefix, const char * defaultSuffix);
bool extractEclCmdOption(StringAttr & option, IProperties * globals, const char * envName, const char * propertyName, const char * defaultPrefix, const char * defaultSuffix);
bool extractEclCmdOption(bool & option, IProperties * globals, const char * envName, const char * propertyName, bool defval);
bool extractEclCmdOption(unsigned & option, IProperties * globals, const char * envName, const char * propertyName, unsigned defval);

enum eclObjParameterType
{
    eclObjTypeUnknown = 0x00,
    eclObjSource = 0x01,
    eclObjArchive = 0x02,
    eclObjSharedObject = 0x04,
    eclObjWuid = 0x08
};

#define eclObjSourceOrArchive (eclObjSource|eclObjArchive)

class EclObjectParameter
{
public:
    EclObjectParameter(unsigned _accept=0x00);
    eclObjParameterType set(const char *_value);
    const char *queryTypeName();
    StringBuffer &getDescription(StringBuffer &s);
    void loadStdIn();
    void loadFile();

    eclObjParameterType finalizeContentType();

    bool isElfContent();
    bool isPEContent();
    void ensureUtf8Content();

public:
    eclObjParameterType type;
    StringAttr value;
    MemoryBuffer mb;
    unsigned accept;
};

enum eclCmdOptionMatchIndicator
{
    EclCmdOptionNoMatch=0,
    EclCmdOptionMatch=1,
    EclCmdOptionCompletion=2
};

class EclCmdCommon : public CInterface, implements IEclCommand
{
public:
    IMPLEMENT_IINTERFACE;
    EclCmdCommon()
    {
    }
    virtual eclCmdOptionMatchIndicator matchCommandLineOption(ArgvIterator &iter, bool finalAttempt=false);
    virtual bool finalizeOptions(IProperties *globals);

    virtual void usage()
    {
        fprintf(stdout,
            "      --server=<ip>        ip of server running ecl services (eclwatch)\n"
            "      --port=<port>        ecl services port\n"
            "      --username=<name>    username for accessing ecl services\n"
            "      --password=<pw>      password for accessing ecl services\n"
        );
    }
public:
    StringAttr optServer;
    StringAttr optPort;
    StringAttr optUsername;
    StringAttr optPassword;
};

#endif
