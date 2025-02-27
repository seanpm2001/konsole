/*
    SPDX-FileCopyrightText: 2007-2008 Robert Knight <robert.knight@gmail.com>
    SPDX-FileCopyrightText: 1997, 1998 Lars Doelle <lars.doelle@on-line.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Own
#include "Vt102Emulation.h"
#include "config-konsole.h"

// Standard
#include <cstdio>
#include <unistd.h>

// Qt
#include <QEvent>
#include <QKeyEvent>
#include <QTimer>

// KDE
#include <KLocalizedString>

// Konsole
#include "EscapeSequenceUrlExtractor.h"
#include "keyboardtranslator/KeyboardTranslator.h"
#include "session/SessionController.h"
#include "terminalDisplay/TerminalDisplay.h"

using Konsole::Vt102Emulation;

/*
   The VT100 has 32 special graphical characters. The usual vt100 extended
   xterm fonts have these at 0x00..0x1f.

   QT's iso mapping leaves 0x00..0x7f without any changes. But the graphicals
   come in here as proper unicode characters.

   We treat non-iso10646 fonts as VT100 extended and do the required mapping
   from unicode to 0x00..0x1f. The remaining translation is then left to the
   QCodec.
*/

// assert for i in [0..31] : vt100extended(vt100_graphics[i]) == i.

/* clang-format off */
unsigned short Konsole::vt100_graphics[32] = {
    // 0/8     1/9    2/10    3/11    4/12    5/13    6/14    7/15
    0x0020, 0x25C6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
    0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
    0xF800, 0xF801, 0x2500, 0xF803, 0xF804, 0x251c, 0x2524, 0x2534,
    0x252c, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00b7
};
/* clang-format on */

enum XTERM_EXTENDED {
    URL_LINK = '8',
};

Vt102Emulation::Vt102Emulation()
    : Emulation()
    , _currentModes(TerminalState())
    , _savedModes(TerminalState())
    , _pendingSessionAttributesUpdates(QHash<int, QString>())
    , _sessionAttributesUpdateTimer(new QTimer(this))
    , _reportFocusEvents(false)
{
    _sessionAttributesUpdateTimer->setSingleShot(true);
    QObject::connect(_sessionAttributesUpdateTimer, &QTimer::timeout, this, &Konsole::Vt102Emulation::updateSessionAttributes);

    initTokenizer();
}

Vt102Emulation::~Vt102Emulation() = default;

void Vt102Emulation::clearEntireScreen()
{
    _currentScreen->clearEntireScreen();
    bufferedUpdate();
}

void Vt102Emulation::reset()
{
    // Save the current codec so we can set it later.
    // Ideally we would want to use the profile setting
    const QTextCodec *currentCodec = codec();

    resetTokenizer();
    resetModes();
    resetCharset(0);
    _screen[0]->reset();
    resetCharset(1);
    _screen[1]->reset();

    if (currentCodec != nullptr) {
        setCodec(currentCodec);
    } else {
        setCodec(LocaleCodec);
    }

    Q_EMIT resetCursorStyleRequest();

    bufferedUpdate();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                     Processing the incoming byte stream                   */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/* Incoming Bytes Event pipeline

   This section deals with decoding the incoming character stream.
   Decoding means here, that the stream is first separated into `tokens'
   which are then mapped to a `meaning' provided as operations by the
   `Screen' class or by the emulation class itself.

   The pipeline proceeds as follows:

   - Tokenizing the ESC codes (onReceiveChar)
   - VT100 code page translation of plain characters (applyCharset)
   - Interpretation of ESC codes (processToken)

   The escape codes and their meaning are described in the
   technical reference of this program.
*/

// Tokens ------------------------------------------------------------------ --

/*
   Since the tokens are the central notion if this section, we've put them
   in front. They provide the syntactical elements used to represent the
   terminals operations as byte sequences.

   They are encodes here into a single machine word, so that we can later
   switch over them easily. Depending on the token itself, additional
   argument variables are filled with parameter values.

   The tokens are defined below:

   - CHR        - Printable characters     (32..255 but DEL (=127))
   - CTL        - Control characters       (0..31 but ESC (= 27), DEL)
   - ESC        - Escape codes of the form <ESC><CHR but `[]()+*#'>
   - ESC_DE     - Escape codes of the form <ESC><any of `()+*#%'> C
   - CSI_PN     - Escape codes of the form <ESC>'['     {Pn} ';' {Pn} C
   - CSI_PS     - Escape codes of the form <ESC>'['     {Pn} ';' ...  C
   - CSI_PR     - Escape codes of the form <ESC>'[' '?' {Pn} ';' ...  C
   - CSI_PE     - Escape codes of the form <ESC>'[' '!' {Pn} ';' ...  C
   - CSI_SP     - Escape codes of the form <ESC>'[' ' ' C
                  (3rd field is a space)
   - CSI_PSP    - Escape codes of the form <ESC>'[' '{Pn}' ' ' C
                  (4th field is a space)
   - VT52       - VT52 escape codes
                  - <ESC><Chr>
                  - <ESC>'Y'{Pc}{Pc}
   - XTE_HA     - Xterm window/terminal attribute commands
                  of the form <ESC>`]' {Pn} `;' {Text} <BEL>
                  (Note that these are handled differently to the other formats)

   The last two forms allow list of arguments. Since the elements of
   the lists are treated individually the same way, they are passed
   as individual tokens to the interpretation. Further, because the
   meaning of the parameters are names (although represented as numbers),
   they are includes within the token ('N').

*/
constexpr int token_construct(int t, int a, int n)
{
    return (((n & 0xffff) << 16) | ((a & 0xff) << 8) | (t & 0xff));
}
constexpr int token_chr()
{
    return token_construct(0, 0, 0);
}
constexpr int token_ctl(int a)
{
    return token_construct(1, a, 0);
}
constexpr int token_esc(int a)
{
    return token_construct(2, a, 0);
}
constexpr int token_esc_cs(int a, int b)
{
    return token_construct(3, a, b);
}
constexpr int token_esc_de(int a)
{
    return token_construct(4, a, 0);
}
constexpr int token_csi_ps(int a, int n)
{
    return token_construct(5, a, n);
}
constexpr int token_csi_pn(int a)
{
    return token_construct(6, a, 0);
}
constexpr int token_csi_pr(int a, int n)
{
    return token_construct(7, a, n);
}
constexpr int token_vt52(int a)
{
    return token_construct(8, a, 0);
}
constexpr int token_csi_pg(int a)
{
    return token_construct(9, a, 0);
}
constexpr int token_csi_pe(int a)
{
    return token_construct(10, a, 0);
}
constexpr int token_csi_sp(int a)
{
    return token_construct(11, a, 0);
}
constexpr int token_csi_psp(int a, int n)
{
    return token_construct(12, a, n);
}
constexpr int token_csi_pq(int a)
{
    return token_construct(13, a, 0);
}

const int MAX_ARGUMENT = 40960;

// Tokenizer --------------------------------------------------------------- --

/* The tokenizer's state

   The state is represented by the buffer (tokenBuffer, tokenBufferPos),
   and accompanied by decoded arguments kept in (argv,argc).
   Note that they are kept internal in the tokenizer.
*/

void Vt102Emulation::resetTokenizer()
{
    tokenBufferPos = 0;
    argc = 0;
    argv[0] = 0;
    argv[1] = 0;
}

void Vt102Emulation::addDigit(int digit)
{
    argv[argc] = qMin(10 * argv[argc] + digit, MAX_ARGUMENT);
}

void Vt102Emulation::addArgument()
{
    argc = qMin(argc + 1, MAXARGS - 1);
    argv[argc] = 0;
}

void Vt102Emulation::addToCurrentToken(uint cc)
{
    tokenBufferPos = qMin(tokenBufferPos, MAX_TOKEN_LENGTH - 1);
    tokenBuffer[tokenBufferPos] = cc;
    tokenBufferPos++;
}

// Character Class flags used while decoding
const int CTL = 1; // Control character
const int CHR = 2; // Printable character
const int CPN = 4; // TODO: Document me
const int DIG = 8; // Digit
const int SCS = 16; // Select Character Set
const int GRP = 32; // TODO: Document me
const int CPS = 64; // Character which indicates end of window resize
const int INT = 128; // Intermediate Byte (ECMA 48 5.4 -> CSI P..P I..I F)

void Vt102Emulation::initTokenizer()
{
    int i;
    quint8 *s;
    for (i = 0; i < 256; ++i) {
        charClass[i] = 0;
    }
    for (i = 0; i < 32; ++i) {
        charClass[i] |= CTL;
    }
    for (i = 32; i < 256; ++i) {
        charClass[i] |= CHR;
    }
    for (i = 0x20; i < 0x30; ++i) {
        charClass[i] |= INT;
    }
    for (s = (quint8 *)"@ABCDEFGHILMPSTXZbcdfry"; *s != 0U; ++s) {
        charClass[*s] |= CPN;
    }
    // resize = \e[8;<row>;<col>t
    for (s = (quint8 *)"t"; *s != 0U; ++s) {
        charClass[*s] |= CPS;
    }
    for (s = (quint8 *)"0123456789"; *s != 0U; ++s) {
        charClass[*s] |= DIG;
    }
    for (s = (quint8 *)"()+*%"; *s != 0U; ++s) {
        charClass[*s] |= SCS;
    }
    for (s = (quint8 *)"()+*#[]%"; *s != 0U; ++s) {
        charClass[*s] |= GRP;
    }

    resetTokenizer();
}

/* Ok, here comes the nasty part of the decoder.

   Instead of keeping an explicit state, we deduce it from the
   token scanned so far. It is then immediately combined with
   the current character to form a scanning decision.

   This is done by the following defines.

   - P is the length of the token scanned so far.
   - L (often P-1) is the position on which contents we base a decision.
   - C is a character or a group of characters (taken from 'charClass').

   - 'cc' is the current character
   - 's' is a pointer to the start of the token buffer
   - 'p' is the current position within the token buffer

   Note that they need to applied in proper order.
*/

/* clang-format off */
#define lec(P,L,C) (p == (P) && s[(L)] == (C))
#define lun(     ) (p ==  1  && cc >= 32 )
#define les(P,L,C) (p == (P) && s[L] < 256 && (charClass[s[(L)]] & (C)) == (C))
#define eec(C)     (p >=  3  && cc == (C))
#define ees(C)     (p >=  3  && cc < 256 && (charClass[cc] & (C)) == (C))
#define eps(C)     (p >=  3  && s[2] != '?' && s[2] != '!' && s[2] != '=' && s[2] != '>' && cc < 256 && (charClass[cc] & (C)) == (C) && (charClass[s[p-2]] & (INT)) != (INT) )
#define epp( )     (p >=  3  && s[2] == '?')
#define epe( )     (p >=  3  && s[2] == '!')
#define eeq( )     (p >=  3  && s[2] == '=')
#define egt( )     (p >=  3  && s[2] == '>')
#define esp( )     (p >=  4  && s[2] == SP )
#define epsp( )    (p >=  5  && s[3] == SP )
#define osc        (tokenBufferPos >= 2 && tokenBuffer[1] == ']')
#define ces(C)     (cc < 256 && (charClass[cc] & (C)) == (C))
#define dcs        (p >= 2   && s[0] == ESC && s[1] == 'P')

/* clang-format on */

#define CNTL(c) ((c) - '@')
const int ESC = 27;
const int DEL = 127;
const int SP = 32;

// process an incoming unicode character
void Vt102Emulation::receiveChars(const QVector<uint> &chars)
{
    for (uint cc : chars) {
        if (cc == DEL) {
            continue; // VT100: ignore.
        }

        if (ces(CTL)) {
            // ignore control characters in the text part of osc (aka OSC) "ESC]"
            // escape sequences; this matches what XTERM docs say
            // Allow BEL and ESC here, it will either end the text or be removed later.
            if (osc && cc != 0x1b && cc != 0x07) {
                continue;
            }

            if (!osc) {
                // DEC HACK ALERT! Control Characters are allowed *within* esc sequences in VT100
                // This means, they do neither a resetTokenizer() nor a pushToToken(). Some of them, do
                // of course. Guess this originates from a weakly layered handling of the X-on
                // X-off protocol, which comes really below this level.
                if (cc == CNTL('X') || cc == CNTL('Z') || cc == ESC) {
                    resetTokenizer(); // VT100: CAN or SUB
                }
                if (cc != ESC) {
                    processToken(token_ctl(cc + '@'), 0, 0);
                    continue;
                }
            }
        }
        // advance the state
        addToCurrentToken(cc);

        uint *s = tokenBuffer;
        const int p = tokenBufferPos;

        if (getMode(MODE_Ansi)) {
            if (lec(1, 0, ESC)) {
                continue;
            }
            if (lec(1, 0, ESC + 128)) {
                s[0] = ESC;
                receiveChars(QVector<uint>{'['});
                continue;
            }
            if (les(2, 1, GRP)) {
                continue;
            }
            // Operating System Command
            if (p > 2 && s[1] == ']') {
                // <ESC> ']' ... <ESC> '\'
                if (s[p - 2] == ESC && s[p - 1] == '\\') {
                    // This runs two times per link, the first prepares the link to be read,
                    // the second finalizes it. The escape sequence is in two parts
                    //  start: '\e ] 8 ; <id-path> ; <url-part> \e \\'
                    //  end:   '\e ] 8 ; ; \e \\'
                    // GNU libtextstyle inserts the IDs, for instance; many examples
                    // do not.
                    if (s[2] == XTERM_EXTENDED::URL_LINK) {
                        // printf '\e]8;;https://example.com\e\\This is a link\e]8;;\e\\\n'
                        _currentScreen->urlExtractor()->toggleUrlInput();
                    }
                    processSessionAttributeRequest(p - 1);
                    resetTokenizer();
                    continue;
                }
                // <ESC> ']' ... <ESC> + one character for reprocessing
                if (s[p - 2] == ESC) {
                    processSessionAttributeRequest(p - 1);
                    resetTokenizer();
                    receiveChars(QVector<uint>{cc});
                    continue;
                }
                // <ESC> ']' ... <BEL>
                if (s[p - 1] == 0x07) {
                    processSessionAttributeRequest(p);
                    resetTokenizer();
                    continue;
                }
            }

            /* clang-format off */
        // <ESC> ']' ...
        if (osc         ) { continue; }
        if (lec(3,2,'?')) { continue; }
        if (lec(3,2,'=')) { continue; }
        if (lec(3,2,'>')) { continue; }
        if (lec(3,2,'!')) { continue; }
        if (lec(3,2,SP )) { continue; }
        if (lec(4,3,SP )) { continue; }
        if (lun(       )) { processToken(token_chr(), applyCharset(cc), 0);   resetTokenizer(); continue; }
        if (dcs         ) { continue; /* TODO We don't xterm DCS, so we just eat it */ }
        if (lec(2,0,ESC)) { processToken(token_esc(s[1]), 0, 0);              resetTokenizer(); continue; }
        if (les(3,1,SCS)) { processToken(token_esc_cs(s[1],s[2]), 0, 0);      resetTokenizer(); continue; }
        if (lec(3,1,'#')) { processToken(token_esc_de(s[2]), 0, 0);           resetTokenizer(); continue; }
        if (eps(    CPN)) { processToken(token_csi_pn(cc), argv[0],argv[1]);  resetTokenizer(); continue; }

        // resize = \e[8;<row>;<col>t
        if (eps(CPS)) {
            processToken(token_csi_ps(cc, argv[0]), argv[1], argv[2]);
            resetTokenizer();
            continue;
        }

        if (epe(   )) { processToken(token_csi_pe(cc), 0, 0); resetTokenizer(); continue; }

        if (esp (   )) { processToken(token_csi_sp(cc), 0, 0);           resetTokenizer(); continue; }
        if (epsp(   )) { processToken(token_csi_psp(cc, argv[0]), 0, 0); resetTokenizer(); continue; }

        if (ees(DIG)) { addDigit(cc-'0'); continue; }
        if (eec(';')) { addArgument();    continue; }
        if (ees(INT)) { continue; }
        if (p >= 3 && cc == 'y' && s[p - 2] == '*') { processChecksumRequest(argc, argv); resetTokenizer(); continue; }
        for (int i = 0; i <= argc; i++) {
            if (epp()) {
                processToken(token_csi_pr(cc,argv[i]), 0, 0);
            } else if (eeq()) {
                processToken(token_csi_pq(cc), 0, 0); // spec. case for ESC[=0c or ESC[=c
            } else if (egt()) {
                processToken(token_csi_pg(cc), 0, 0); // spec. case for ESC[>0c or ESC[>c
            } else if (cc == 'm' && argc - i >= 4 && (argv[i] == 38 || argv[i] == 48) && argv[i+1] == 2)
            {
                // ESC[ ... 48;2;<red>;<green>;<blue> ... m -or- ESC[ ... 38;2;<red>;<green>;<blue> ... m
                i += 2;
                processToken(token_csi_ps(cc, argv[i-2]), COLOR_SPACE_RGB, (argv[i] << 16) | (argv[i+1] << 8) | argv[i+2]);
                i += 2;
            } else if (cc == 'm' && argc - i >= 2 && (argv[i] == 38 || argv[i] == 48) && argv[i+1] == 5) {
                // ESC[ ... 48;5;<index> ... m -or- ESC[ ... 38;5;<index> ... m
                i += 2;
                processToken(token_csi_ps(cc, argv[i-2]), COLOR_SPACE_256, argv[i]);
            } else if (p < 2 || (charClass[s[p-2]] & (INT)) != (INT)) {
                processToken(token_csi_ps(cc,argv[i]), 0, 0);
            }
        }
        resetTokenizer();
        /* clang-format on */
        } else {
            // VT52 Mode
            if (lec(1, 0, ESC)) {
                continue;
            }
            if (les(1, 0, CHR)) {
                processToken(token_chr(), s[0], 0);
                resetTokenizer();
                continue;
            }
            if (lec(2, 1, 'Y')) {
                continue;
            }
            if (lec(3, 1, 'Y')) {
                continue;
            }
            if (p < 4) {
                processToken(token_vt52(s[1]), 0, 0);
                resetTokenizer();
                continue;
            }
            processToken(token_vt52(s[1]), s[2], s[3]);
            resetTokenizer();
            continue;
        }
    }
}

void Vt102Emulation::processChecksumRequest([[maybe_unused]] int argc, int argv[])
{
    int checksum = 0;

#if defined(ENABLE_DECRQCRA)
    int top, left, bottom, right;

    /* DEC STD-070 5-179 "If Pp is 0 or omitted, subsequent parameters are ignored
     *  and a checksum for all page memory will be reported."
     */
    if (argv[1] == 0) {
        argc = 1;
    }
    /* clang-format off */
    if (argc >= 2) { top    = argv[2]; } else { top    = 1; }
    if (argc >= 3) { left   = argv[3]; } else { left   = 1; }
    if (argc >= 4) { bottom = argv[4]; } else { bottom = _currentScreen->getLines();   }
    if (argc >= 5) { right  = argv[5]; } else { right  = _currentScreen->getColumns(); }
    /* clang-format on */

    if (top > bottom || left > right) {
        return;
    }

    if (_currentScreen->getMode(MODE_Origin)) {
        top += _currentScreen->topMargin();
        bottom += _currentScreen->topMargin();
    }

    top = qBound(1, top, _currentScreen->getLines());
    bottom = qBound(1, bottom, _currentScreen->getLines());

    int imgsize = sizeof(Character) * _currentScreen->getLines() * _currentScreen->getColumns();
    Character *image = (Character *)malloc(imgsize);
    if (image == nullptr) {
        fprintf(stderr, "couldn't alloc mem\n");
        return;
    }
    _currentScreen->getImage(image, imgsize, _currentScreen->getHistLines(), _currentScreen->getHistLines() + _currentScreen->getLines() - 1);

    for (int y = top - 1; y <= bottom - 1; y++) {
        for (int x = left - 1; x <= right - 1; x++) {
            // XXX: Apparently, VT520 uses 0x00 for uninitialized cells, konsole can't tell uninitialized cells from spaces
            Character c = image[y * _currentScreen->getColumns() + x];

            if (c.rendition & RE_CONCEAL) {
                checksum += 0x20; // don't reveal secrets
            } else {
                checksum += c.character;
            }

            checksum += (c.rendition & RE_BOLD) / RE_BOLD * 0x80;
            checksum += (c.rendition & RE_BLINK) / RE_BLINK * 0x40;
            checksum += (c.rendition & RE_REVERSE) / RE_REVERSE * 0x20;
            checksum += (c.rendition & RE_UNDERLINE) / RE_UNDERLINE * 0x10;
        }
    }

    free(image);
#endif

    char tmp[30];
    checksum = -checksum;
    checksum &= 0xffff;
    snprintf(tmp, sizeof(tmp), "\033P%d!~%04X\033\\", argv[0], checksum);
    sendString(tmp);
}

void Vt102Emulation::processSessionAttributeRequest(int tokenSize)
{
    // Describes the window or terminal session attribute to change
    // See Session::SessionAttributes for possible values
    int attribute = 0;
    int i;

    // ignore last character (ESC or BEL)
    --tokenSize;

    /* clang-format off */
    // skip first two characters (ESC, ']')
    for (i = 2; i < tokenSize &&
                tokenBuffer[i] >= '0'  &&
                tokenBuffer[i] <= '9'; i++)
    {
        attribute = 10 * attribute + (tokenBuffer[i]-'0');
    }
    /* clang-format on */

    if (tokenBuffer[i] != ';') {
        reportDecodingError();
        return;
    }
    // skip initial ';'
    ++i;

    QString value = QString::fromUcs4(&tokenBuffer[i], tokenSize - i);
    if (_currentScreen->urlExtractor()->reading()) {
        // To handle '\e ] 8 ; <id-part> ; <url-part>' we discard
        // the <id-part>. Often it is empty, but GNU libtextstyle
        // may output an id here, see e.g.
        // https://www.gnu.org/software/gettext/libtextstyle/manual/libtextstyle.html#index-styled_005fostream_005fset_005fhyperlink
        value.remove(0, value.indexOf(QLatin1Char(';'))+1);
        _currentScreen->urlExtractor()->setUrl(value);
        return;
    }

    if (value == QLatin1String("?")) {
        // pass terminator type indication here, because OSC response terminator
        // should match the terminator of OSC request.
        Q_EMIT sessionAttributeRequest(attribute, tokenBuffer[tokenSize]);
        return;
    }

    if (attribute == Session::ProfileChange) {
        if (value.startsWith(QLatin1String("CursorShape="))) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const auto numStr = QStringView(value).right(1);
#else
            const auto numStr = value.rightRef(1);
#endif
            const Enum::CursorShapeEnum shape = static_cast<Enum::CursorShapeEnum>(numStr.toInt());
            Q_EMIT setCursorStyleRequest(shape);
            return;
        }
    }

    _pendingSessionAttributesUpdates[attribute] = value;
    _sessionAttributesUpdateTimer->start(20);
}

void Vt102Emulation::updateSessionAttributes()
{
    QListIterator<int> iter(_pendingSessionAttributesUpdates.keys());
    while (iter.hasNext()) {
        int arg = iter.next();
        Q_EMIT sessionAttributeChanged(arg, _pendingSessionAttributesUpdates[arg]);
    }
    _pendingSessionAttributesUpdates.clear();
}

// Interpreting Codes ---------------------------------------------------------

/*
   Now that the incoming character stream is properly tokenized,
   meaning is assigned to them. These are either operations of
   the current _screen, or of the emulation class itself.

   The token to be interpreted comes in as a machine word
   possibly accompanied by two parameters.

   Likewise, the operations assigned to, come with up to two
   arguments. One could consider to make up a proper table
   from the function below.

   The technical reference manual provides more information
   about this mapping.
*/

void Vt102Emulation::processToken(int token, int p, int q)
{
    /* clang-format off */
    switch (token) {
    case token_chr(         ) :
        _currentScreen->displayCharacter     (p         ); break; //UTF16

    //             127 DEL    : ignored on input

    case token_ctl('@'      ) : /* NUL: ignored                      */ break;
    case token_ctl('A'      ) : /* SOH: ignored                      */ break;
    case token_ctl('B'      ) : /* STX: ignored                      */ break;
    case token_ctl('C'      ) : /* ETX: ignored                      */ break;
    case token_ctl('D'      ) : /* EOT: ignored                      */ break;
    case token_ctl('E'      ) :      reportAnswerBack     (          ); break; //VT100
    case token_ctl('F'      ) : /* ACK: ignored                      */ break;
    case token_ctl('G'      ) : Q_EMIT bell();
                                break; //VT100
    case token_ctl('H'      ) : _currentScreen->backspace            (          ); break; //VT100
    case token_ctl('I'      ) : _currentScreen->tab                  (          ); break; //VT100
    case token_ctl('J'      ) : _currentScreen->newLine              (          ); break; //VT100
    case token_ctl('K'      ) : _currentScreen->newLine              (          ); break; //VT100
    case token_ctl('L'      ) : _currentScreen->newLine              (          ); break; //VT100
    case token_ctl('M'      ) : _currentScreen->toStartOfLine        (          ); break; //VT100

    case token_ctl('N'      ) :      useCharset           (         1); break; //VT100
    case token_ctl('O'      ) :      useCharset           (         0); break; //VT100

    case token_ctl('P'      ) : /* DLE: ignored                      */ break;
    case token_ctl('Q'      ) : /* DC1: XON continue                 */ break; //VT100
    case token_ctl('R'      ) : /* DC2: ignored                      */ break;
    case token_ctl('S'      ) : /* DC3: XOFF halt                    */ break; //VT100
    case token_ctl('T'      ) : /* DC4: ignored                      */ break;
    case token_ctl('U'      ) : /* NAK: ignored                      */ break;
    case token_ctl('V'      ) : /* SYN: ignored                      */ break;
    case token_ctl('W'      ) : /* ETB: ignored                      */ break;
    case token_ctl('X'      ) : _currentScreen->displayCharacter     (    0x2592); break; //VT100
    case token_ctl('Y'      ) : /* EM : ignored                      */ break;
    case token_ctl('Z'      ) : _currentScreen->displayCharacter     (    0x2592); break; //VT100
    case token_ctl('['      ) : /* ESC: cannot be seen here.         */ break;
    case token_ctl('\\'     ) : /* FS : ignored                      */ break;
    case token_ctl(']'      ) : /* GS : ignored                      */ break;
    case token_ctl('^'      ) : /* RS : ignored                      */ break;
    case token_ctl('_'      ) : /* US : ignored                      */ break;

    case token_esc('D'      ) : _currentScreen->index                (          ); break; //VT100
    case token_esc('E'      ) : _currentScreen->nextLine             (          ); break; //VT100
    case token_esc('H'      ) : _currentScreen->changeTabStop        (true      ); break; //VT100
    case token_esc('M'      ) : _currentScreen->reverseIndex         (          ); break; //VT100
    case token_esc('Z'      ) :      reportTerminalType   (          ); break;
    case token_esc('c'      ) :      reset                (          ); break;

    case token_esc('n'      ) :      useCharset           (         2); break;
    case token_esc('o'      ) :      useCharset           (         3); break;
    case token_esc('7'      ) :      saveCursor           (          ); break;
    case token_esc('8'      ) :      restoreCursor        (          ); break;

    case token_esc('='      ) :          setMode      (MODE_AppKeyPad); break;
    case token_esc('>'      ) :        resetMode      (MODE_AppKeyPad); break;
    case token_esc('<'      ) :          setMode      (MODE_Ansi     ); break; //VT100

    case token_esc_cs('(', '0') :      setCharset           (0,    '0'); break; //VT100
    case token_esc_cs('(', 'A') :      setCharset           (0,    'A'); break; //VT100
    case token_esc_cs('(', 'B') :      setCharset           (0,    'B'); break; //VT100

    case token_esc_cs(')', '0') :      setCharset           (1,    '0'); break; //VT100
    case token_esc_cs(')', 'A') :      setCharset           (1,    'A'); break; //VT100
    case token_esc_cs(')', 'B') :      setCharset           (1,    'B'); break; //VT100

    case token_esc_cs('*', '0') :      setCharset           (2,    '0'); break; //VT100
    case token_esc_cs('*', 'A') :      setCharset           (2,    'A'); break; //VT100
    case token_esc_cs('*', 'B') :      setCharset           (2,    'B'); break; //VT100

    case token_esc_cs('+', '0') :      setCharset           (3,    '0'); break; //VT100
    case token_esc_cs('+', 'A') :      setCharset           (3,    'A'); break; //VT100
    case token_esc_cs('+', 'B') :      setCharset           (3,    'B'); break; //VT100

    case token_esc_cs('%', 'G') :      setCodec             (Utf8Codec   ); break; //LINUX
    case token_esc_cs('%', '@') :      setCodec             (LocaleCodec ); break; //LINUX

    case token_esc_de('3'      ) : /* Double height line, top half    */
                                _currentScreen->setLineProperty( LINE_DOUBLEWIDTH , true );
                                _currentScreen->setLineProperty( LINE_DOUBLEHEIGHT_TOP , true );
                                _currentScreen->setLineProperty( LINE_DOUBLEHEIGHT_BOTTOM , false );
                                    break;
    case token_esc_de('4'      ) : /* Double height line, bottom half */
                                _currentScreen->setLineProperty( LINE_DOUBLEWIDTH , true );
                                _currentScreen->setLineProperty( LINE_DOUBLEHEIGHT_TOP , false );
                                _currentScreen->setLineProperty( LINE_DOUBLEHEIGHT_BOTTOM , true );
                                    break;
    case token_esc_de('5'      ) : /* Single width, single height line*/
                                _currentScreen->setLineProperty( LINE_DOUBLEWIDTH , false);
                                _currentScreen->setLineProperty( LINE_DOUBLEHEIGHT_TOP , false);
                                _currentScreen->setLineProperty( LINE_DOUBLEHEIGHT_BOTTOM , false);
                                break;
    case token_esc_de('6'      ) : /* Double width, single height line*/
                                _currentScreen->setLineProperty( LINE_DOUBLEWIDTH , true);
                                _currentScreen->setLineProperty( LINE_DOUBLEHEIGHT_TOP , false);
                                _currentScreen->setLineProperty( LINE_DOUBLEHEIGHT_BOTTOM , false);
                                break;
    case token_esc_de('8'      ) : _currentScreen->helpAlign            (          ); break;

    // resize = \e[8;<rows>;<cols>t
    case token_csi_ps('t',   8) : setImageSize( p /* rows */, q /* columns */ );
                               Q_EMIT imageResizeRequest(QSize(q, p)); // Note columns (x), rows (y) in QSize
                               break;

    case token_csi_ps('t',   18) : reportSize();          break;
    // change tab text color : \e[28;<color>t  color: 0-16,777,215
    case token_csi_ps('t',   28) : /* IGNORED: konsole-specific KDE3-era extension, not implemented */ break;

    case token_csi_ps('t',  22) : /* IGNORED: Save icon and window title on stack */      break; //XTERM
    case token_csi_ps('t',  23) : /* IGNORED: Restore icon and window title from stack */ break; //XTERM

    case token_csi_ps('K',   0) : _currentScreen->clearToEndOfLine     (          ); break;
    case token_csi_ps('K',   1) : _currentScreen->clearToBeginOfLine   (          ); break;
    case token_csi_ps('K',   2) : _currentScreen->clearEntireLine      (          ); break;
    case token_csi_ps('J',   0) : _currentScreen->clearToEndOfScreen   (          ); break;
    case token_csi_ps('J',   1) : _currentScreen->clearToBeginOfScreen (          ); break;
    case token_csi_ps('J',   2) : _currentScreen->clearEntireScreen    (          ); break;
    case token_csi_ps('J',      3) : clearHistory();                            break;
    case token_csi_ps('g',   0) : _currentScreen->changeTabStop        (false     ); break; //VT100
    case token_csi_ps('g',   3) : _currentScreen->clearTabStops        (          ); break; //VT100
    case token_csi_ps('h',   4) : _currentScreen->    setMode      (MODE_Insert   ); break;
    case token_csi_ps('h',  20) :          setMode      (MODE_NewLine  ); break;
    case token_csi_ps('i',   0) : /* IGNORE: attached printer          */ break; //VT100
    case token_csi_ps('l',   4) : _currentScreen->  resetMode      (MODE_Insert   ); break;
    case token_csi_ps('l',  20) :        resetMode      (MODE_NewLine  ); break;
    case token_csi_ps('s',   0) :      saveCursor           (          ); break;
    case token_csi_ps('u',   0) :      restoreCursor        (          ); break;

    case token_csi_ps('m',   0) : _currentScreen->setDefaultRendition  (          ); break;
    case token_csi_ps('m',   1) : _currentScreen->  setRendition     (RE_BOLD     ); break; //VT100
    case token_csi_ps('m',   2) : _currentScreen->  setRendition     (RE_FAINT    ); break;
    case token_csi_ps('m',   3) : _currentScreen->  setRendition     (RE_ITALIC   ); break; //VT100
    case token_csi_ps('m',   4) : _currentScreen->  setRendition     (RE_UNDERLINE); break; //VT100
    case token_csi_ps('m',   5) : _currentScreen->  setRendition     (RE_BLINK    ); break; //VT100
    case token_csi_ps('m',   7) : _currentScreen->  setRendition     (RE_REVERSE  ); break;
    case token_csi_ps('m',   8) : _currentScreen->  setRendition     (RE_CONCEAL  ); break;
    case token_csi_ps('m',   9) : _currentScreen->  setRendition     (RE_STRIKEOUT); break;
    case token_csi_ps('m',  53) : _currentScreen->  setRendition     (RE_OVERLINE ); break;
    case token_csi_ps('m',  10) : /* IGNORED: mapping related          */ break; //LINUX
    case token_csi_ps('m',  11) : /* IGNORED: mapping related          */ break; //LINUX
    case token_csi_ps('m',  12) : /* IGNORED: mapping related          */ break; //LINUX
    case token_csi_ps('m',  21) : _currentScreen->resetRendition     (RE_BOLD     ); break;
    case token_csi_ps('m',  22) : _currentScreen->resetRendition     (RE_BOLD     );
                               _currentScreen->resetRendition     (RE_FAINT    ); break;
    case token_csi_ps('m',  23) : _currentScreen->resetRendition     (RE_ITALIC   ); break; //VT100
    case token_csi_ps('m',  24) : _currentScreen->resetRendition     (RE_UNDERLINE); break;
    case token_csi_ps('m',  25) : _currentScreen->resetRendition     (RE_BLINK    ); break;
    case token_csi_ps('m',  27) : _currentScreen->resetRendition     (RE_REVERSE  ); break;
    case token_csi_ps('m',  28) : _currentScreen->resetRendition     (RE_CONCEAL  ); break;
    case token_csi_ps('m',  29) : _currentScreen->resetRendition     (RE_STRIKEOUT); break;
    case token_csi_ps('m',  55) : _currentScreen->resetRendition     (RE_OVERLINE ); break;

    case token_csi_ps('m',   30) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM,  0); break;
    case token_csi_ps('m',   31) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM,  1); break;
    case token_csi_ps('m',   32) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM,  2); break;
    case token_csi_ps('m',   33) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM,  3); break;
    case token_csi_ps('m',   34) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM,  4); break;
    case token_csi_ps('m',   35) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM,  5); break;
    case token_csi_ps('m',   36) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM,  6); break;
    case token_csi_ps('m',   37) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM,  7); break;

    case token_csi_ps('m',   38) : _currentScreen->setForeColor         (p,       q); break;

    case token_csi_ps('m',   39) : _currentScreen->setForeColor         (COLOR_SPACE_DEFAULT,  0); break;

    case token_csi_ps('m',   40) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM,  0); break;
    case token_csi_ps('m',   41) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM,  1); break;
    case token_csi_ps('m',   42) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM,  2); break;
    case token_csi_ps('m',   43) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM,  3); break;
    case token_csi_ps('m',   44) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM,  4); break;
    case token_csi_ps('m',   45) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM,  5); break;
    case token_csi_ps('m',   46) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM,  6); break;
    case token_csi_ps('m',   47) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM,  7); break;

    case token_csi_ps('m',   48) : _currentScreen->setBackColor         (p,       q); break;

    case token_csi_ps('m',   49) : _currentScreen->setBackColor         (COLOR_SPACE_DEFAULT,  1); break;

    case token_csi_ps('m',   90) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM,  8); break;
    case token_csi_ps('m',   91) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM,  9); break;
    case token_csi_ps('m',   92) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM, 10); break;
    case token_csi_ps('m',   93) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM, 11); break;
    case token_csi_ps('m',   94) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM, 12); break;
    case token_csi_ps('m',   95) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM, 13); break;
    case token_csi_ps('m',   96) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM, 14); break;
    case token_csi_ps('m',   97) : _currentScreen->setForeColor         (COLOR_SPACE_SYSTEM, 15); break;

    case token_csi_ps('m',  100) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM,  8); break;
    case token_csi_ps('m',  101) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM,  9); break;
    case token_csi_ps('m',  102) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM, 10); break;
    case token_csi_ps('m',  103) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM, 11); break;
    case token_csi_ps('m',  104) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM, 12); break;
    case token_csi_ps('m',  105) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM, 13); break;
    case token_csi_ps('m',  106) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM, 14); break;
    case token_csi_ps('m',  107) : _currentScreen->setBackColor         (COLOR_SPACE_SYSTEM, 15); break;

    case token_csi_ps('n',   5) :      reportStatus         (          ); break;
    case token_csi_ps('n',   6) :      reportCursorPosition (          ); break;
    case token_csi_ps('q',   0) : /* IGNORED: LEDs off                 */ break; //VT100
    case token_csi_ps('q',   1) : /* IGNORED: LED1 on                  */ break; //VT100
    case token_csi_ps('q',   2) : /* IGNORED: LED2 on                  */ break; //VT100
    case token_csi_ps('q',   3) : /* IGNORED: LED3 on                  */ break; //VT100
    case token_csi_ps('q',   4) : /* IGNORED: LED4 on                  */ break; //VT100
    case token_csi_ps('x',   0) :      reportTerminalParms  (         2); break; //VT100
    case token_csi_ps('x',   1) :      reportTerminalParms  (         3); break; //VT100

    case token_csi_pn('@'      ) : _currentScreen->insertChars          (p         ); break;
    case token_csi_pn('A'      ) : _currentScreen->cursorUp             (p         ); break; //VT100
    case token_csi_pn('B'      ) : _currentScreen->cursorDown           (p         ); break; //VT100
    case token_csi_pn('C'      ) : _currentScreen->cursorRight          (p         ); break; //VT100
    case token_csi_pn('D'      ) : _currentScreen->cursorLeft           (p         ); break; //VT100
    case token_csi_pn('E'      ) : _currentScreen->cursorNextLine       (p         ); break; //VT100
    case token_csi_pn('F'      ) : _currentScreen->cursorPreviousLine   (p         ); break; //VT100
    case token_csi_pn('G'      ) : _currentScreen->setCursorX           (p         ); break; //LINUX
    case token_csi_pn('H'      ) : _currentScreen->setCursorYX          (p,      q); break; //VT100
    case token_csi_pn('I'      ) : _currentScreen->tab                  (p         ); break;
    case token_csi_pn('L'      ) : _currentScreen->insertLines          (p         ); break;
    case token_csi_pn('M'      ) : _currentScreen->deleteLines          (p         ); break;
    case token_csi_pn('P'      ) : _currentScreen->deleteChars          (p         ); break;
    case token_csi_pn('S'      ) : _currentScreen->scrollUp             (p         ); break;
    case token_csi_pn('T'      ) : _currentScreen->scrollDown           (p         ); break;
    case token_csi_pn('X'      ) : _currentScreen->eraseChars           (p         ); break;
    case token_csi_pn('Z'      ) : _currentScreen->backtab              (p         ); break;
    case token_csi_pn('b'      ) : _currentScreen->repeatChars          (p         ); break;
    case token_csi_pn('c'      ) :      reportTerminalType   (          ); break; //VT100
    case token_csi_pn('d'      ) : _currentScreen->setCursorY           (p         ); break; //LINUX
    case token_csi_pn('f'      ) : _currentScreen->setCursorYX          (p,      q); break; //VT100
    case token_csi_pn('r'      ) :      setMargins           (p,      q); break; //VT100
    case token_csi_pn('y'      ) : /* IGNORED: Confidence test          */ break; //VT100

    case token_csi_pr('h',   1) :          setMode      (MODE_AppCuKeys); break; //VT100
    case token_csi_pr('l',   1) :        resetMode      (MODE_AppCuKeys); break; //VT100
    case token_csi_pr('s',   1) :         saveMode      (MODE_AppCuKeys); break; //FIXME
    case token_csi_pr('r',   1) :      restoreMode      (MODE_AppCuKeys); break; //FIXME

    case token_csi_pr('l',   2) :        resetMode      (MODE_Ansi     ); break; //VT100

    case token_csi_pr('h',   3) :          setMode      (MODE_132Columns); break; //VT100
    case token_csi_pr('l',   3) :        resetMode      (MODE_132Columns); break; //VT100

    case token_csi_pr('h',   4) : /* IGNORED: soft scrolling           */ break; //VT100
    case token_csi_pr('l',   4) : /* IGNORED: soft scrolling           */ break; //VT100

    case token_csi_pr('h',   5) : _currentScreen->    setMode      (MODE_Screen   ); break; //VT100
    case token_csi_pr('l',   5) : _currentScreen->  resetMode      (MODE_Screen   ); break; //VT100

    case token_csi_pr('h',   6) : _currentScreen->    setMode      (MODE_Origin   ); break; //VT100
    case token_csi_pr('l',   6) : _currentScreen->  resetMode      (MODE_Origin   ); break; //VT100
    case token_csi_pr('s',   6) : _currentScreen->   saveMode      (MODE_Origin   ); break; //FIXME
    case token_csi_pr('r',   6) : _currentScreen->restoreMode      (MODE_Origin   ); break; //FIXME

    case token_csi_pr('h',   7) : _currentScreen->    setMode      (MODE_Wrap     ); break; //VT100
    case token_csi_pr('l',   7) : _currentScreen->  resetMode      (MODE_Wrap     ); break; //VT100
    case token_csi_pr('s',   7) : _currentScreen->   saveMode      (MODE_Wrap     ); break; //FIXME
    case token_csi_pr('r',   7) : _currentScreen->restoreMode      (MODE_Wrap     ); break; //FIXME

    case token_csi_pr('h',   8) : /* IGNORED: autorepeat on            */ break; //VT100
    case token_csi_pr('l',   8) : /* IGNORED: autorepeat off           */ break; //VT100
    case token_csi_pr('s',   8) : /* IGNORED: autorepeat on            */ break; //VT100
    case token_csi_pr('r',   8) : /* IGNORED: autorepeat off           */ break; //VT100

    case token_csi_pr('h',   9) : /* IGNORED: interlace                */ break; //VT100
    case token_csi_pr('l',   9) : /* IGNORED: interlace                */ break; //VT100
    case token_csi_pr('s',   9) : /* IGNORED: interlace                */ break; //VT100
    case token_csi_pr('r',   9) : /* IGNORED: interlace                */ break; //VT100

    case token_csi_pr('h',  12) : /* IGNORED: Cursor blink             */ break; //att610
    case token_csi_pr('l',  12) : /* IGNORED: Cursor blink             */ break; //att610
    case token_csi_pr('s',  12) : /* IGNORED: Cursor blink             */ break; //att610
    case token_csi_pr('r',  12) : /* IGNORED: Cursor blink             */ break; //att610

    case token_csi_pr('h',  25) :          setMode      (MODE_Cursor   ); break; //VT100
    case token_csi_pr('l',  25) :        resetMode      (MODE_Cursor   ); break; //VT100
    case token_csi_pr('s',  25) :         saveMode      (MODE_Cursor   ); break; //VT100
    case token_csi_pr('r',  25) :      restoreMode      (MODE_Cursor   ); break; //VT100

    case token_csi_pr('h',  40) :         setMode(MODE_Allow132Columns ); break; // XTERM
    case token_csi_pr('l',  40) :       resetMode(MODE_Allow132Columns ); break; // XTERM

    case token_csi_pr('h',  41) : /* IGNORED: obsolete more(1) fix     */ break; //XTERM
    case token_csi_pr('l',  41) : /* IGNORED: obsolete more(1) fix     */ break; //XTERM
    case token_csi_pr('s',  41) : /* IGNORED: obsolete more(1) fix     */ break; //XTERM
    case token_csi_pr('r',  41) : /* IGNORED: obsolete more(1) fix     */ break; //XTERM

    case token_csi_pr('h',  47) :          setMode      (MODE_AppScreen); break; //VT100
    case token_csi_pr('l',  47) :        resetMode      (MODE_AppScreen); break; //VT100
    case token_csi_pr('s',  47) :         saveMode      (MODE_AppScreen); break; //XTERM
    case token_csi_pr('r',  47) :      restoreMode      (MODE_AppScreen); break; //XTERM

    case token_csi_pr('h',  67) : /* IGNORED: DECBKM                   */ break; //XTERM
    case token_csi_pr('l',  67) : /* IGNORED: DECBKM                   */ break; //XTERM
    case token_csi_pr('s',  67) : /* IGNORED: DECBKM                   */ break; //XTERM
    case token_csi_pr('r',  67) : /* IGNORED: DECBKM                   */ break; //XTERM

    // XTerm defines the following modes:
    // SET_VT200_MOUSE             1000
    // SET_VT200_HIGHLIGHT_MOUSE   1001
    // SET_BTN_EVENT_MOUSE         1002
    // SET_ANY_EVENT_MOUSE         1003
    //

    //Note about mouse modes:
    //There are four mouse modes which xterm-compatible terminals can support - 1000,1001,1002,1003
    //Konsole currently supports mode 1000 (basic mouse press and release),  mode 1002 (dragging the mouse)
    //and mode 1003 (moving the mouse).
    //TODO:  Implementation of mouse mode 1001 (something called highlight tracking).
    //

    case token_csi_pr('h', 1000) :          setMode      (MODE_Mouse1000); break; //XTERM
    case token_csi_pr('l', 1000) :        resetMode      (MODE_Mouse1000); break; //XTERM
    case token_csi_pr('s', 1000) :         saveMode      (MODE_Mouse1000); break; //XTERM
    case token_csi_pr('r', 1000) :      restoreMode      (MODE_Mouse1000); break; //XTERM

    case token_csi_pr('h', 1001) : /* IGNORED: hilite mouse tracking    */ break; //XTERM
    case token_csi_pr('l', 1001) :        resetMode      (MODE_Mouse1001); break; //XTERM
    case token_csi_pr('s', 1001) : /* IGNORED: hilite mouse tracking    */ break; //XTERM
    case token_csi_pr('r', 1001) : /* IGNORED: hilite mouse tracking    */ break; //XTERM

    case token_csi_pr('h', 1002) :          setMode      (MODE_Mouse1002); break; //XTERM
    case token_csi_pr('l', 1002) :        resetMode      (MODE_Mouse1002); break; //XTERM
    case token_csi_pr('s', 1002) :         saveMode      (MODE_Mouse1002); break; //XTERM
    case token_csi_pr('r', 1002) :      restoreMode      (MODE_Mouse1002); break; //XTERM

    case token_csi_pr('h', 1003) :          setMode      (MODE_Mouse1003); break; //XTERM
    case token_csi_pr('l', 1003) :        resetMode      (MODE_Mouse1003); break; //XTERM
    case token_csi_pr('s', 1003) :         saveMode      (MODE_Mouse1003); break; //XTERM
    case token_csi_pr('r', 1003) :      restoreMode      (MODE_Mouse1003); break; //XTERM

    case token_csi_pr('h',  1004) : _reportFocusEvents = true; break;
    case token_csi_pr('l',  1004) : _reportFocusEvents = false; break;

    case token_csi_pr('h', 1005) :          setMode      (MODE_Mouse1005); break; //XTERM
    case token_csi_pr('l', 1005) :        resetMode      (MODE_Mouse1005); break; //XTERM
    case token_csi_pr('s', 1005) :         saveMode      (MODE_Mouse1005); break; //XTERM
    case token_csi_pr('r', 1005) :      restoreMode      (MODE_Mouse1005); break; //XTERM

    case token_csi_pr('h', 1006) :          setMode      (MODE_Mouse1006); break; //XTERM
    case token_csi_pr('l', 1006) :        resetMode      (MODE_Mouse1006); break; //XTERM
    case token_csi_pr('s', 1006) :         saveMode      (MODE_Mouse1006); break; //XTERM
    case token_csi_pr('r', 1006) :      restoreMode      (MODE_Mouse1006); break; //XTERM

    case token_csi_pr('h', 1007) :          setMode      (MODE_Mouse1007); break; //XTERM
    case token_csi_pr('l', 1007) :        resetMode      (MODE_Mouse1007); break; //XTERM
    case token_csi_pr('s', 1007) :         saveMode      (MODE_Mouse1007); break; //XTERM
    case token_csi_pr('r', 1007) :      restoreMode      (MODE_Mouse1007); break; //XTERM

    case token_csi_pr('h', 1015) :          setMode      (MODE_Mouse1015); break; //URXVT
    case token_csi_pr('l', 1015) :        resetMode      (MODE_Mouse1015); break; //URXVT
    case token_csi_pr('s', 1015) :         saveMode      (MODE_Mouse1015); break; //URXVT
    case token_csi_pr('r', 1015) :      restoreMode      (MODE_Mouse1015); break; //URXVT

    case token_csi_pr('h', 1034) : /* IGNORED: 8bitinput activation     */ break; //XTERM

    case token_csi_pr('h', 1047) :          setMode      (MODE_AppScreen); break; //XTERM
    case token_csi_pr('l', 1047) : _screen[1]->clearEntireScreen(); resetMode(MODE_AppScreen); break; //XTERM
    case token_csi_pr('s', 1047) :         saveMode      (MODE_AppScreen); break; //XTERM
    case token_csi_pr('r', 1047) :      restoreMode      (MODE_AppScreen); break; //XTERM

    //FIXME: Unitoken: save translations
    case token_csi_pr('h', 1048) :      saveCursor           (          ); break; //XTERM
    case token_csi_pr('l', 1048) :      restoreCursor        (          ); break; //XTERM
    case token_csi_pr('s', 1048) :      saveCursor           (          ); break; //XTERM
    case token_csi_pr('r', 1048) :      restoreCursor        (          ); break; //XTERM

    //FIXME: every once new sequences like this pop up in xterm.
    //       Here's a guess of what they could mean.
    case token_csi_pr('h', 1049) : saveCursor(); _screen[1]->clearEntireScreen(); setMode(MODE_AppScreen); break; //XTERM
    case token_csi_pr('l', 1049) : resetMode(MODE_AppScreen); restoreCursor(); break; //XTERM

    case token_csi_pr('h', 2004) :          setMode      (MODE_BracketedPaste); break; //XTERM
    case token_csi_pr('l', 2004) :        resetMode      (MODE_BracketedPaste); break; //XTERM
    case token_csi_pr('s', 2004) :         saveMode      (MODE_BracketedPaste); break; //XTERM
    case token_csi_pr('r', 2004) :      restoreMode      (MODE_BracketedPaste); break; //XTERM

    // Set Cursor Style (DECSCUSR), VT520, with the extra xterm sequences
    // the first one is a special case, 'ESC[ q', which mimics 'ESC[1 q'
    // Using 0 to reset to default is matching VTE, but not any official standard.
    case token_csi_sp ('q'    ) : Q_EMIT setCursorStyleRequest(Enum::BlockCursor,     true);  break;
    case token_csi_psp('q',  0) : Q_EMIT resetCursorStyleRequest();                           break;
    case token_csi_psp('q',  1) : Q_EMIT setCursorStyleRequest(Enum::BlockCursor,     true);  break;
    case token_csi_psp('q',  2) : Q_EMIT setCursorStyleRequest(Enum::BlockCursor,     false); break;
    case token_csi_psp('q',  3) : Q_EMIT setCursorStyleRequest(Enum::UnderlineCursor, true);  break;
    case token_csi_psp('q',  4) : Q_EMIT setCursorStyleRequest(Enum::UnderlineCursor, false); break;
    case token_csi_psp('q',  5) : Q_EMIT setCursorStyleRequest(Enum::IBeamCursor,     true);  break;
    case token_csi_psp('q',  6) : Q_EMIT setCursorStyleRequest(Enum::IBeamCursor,     false); break;

    //FIXME: weird DEC reset sequence
    case token_csi_pe('p'      ) : /* IGNORED: reset         (        ) */ break;

    //FIXME: when changing between vt52 and ansi mode evtl do some resetting.
    case token_vt52('A'      ) : _currentScreen->cursorUp             (         1); break; //VT52
    case token_vt52('B'      ) : _currentScreen->cursorDown           (         1); break; //VT52
    case token_vt52('C'      ) : _currentScreen->cursorRight          (         1); break; //VT52
    case token_vt52('D'      ) : _currentScreen->cursorLeft           (         1); break; //VT52

    case token_vt52('F'      ) :      setAndUseCharset     (0,    '0'); break; //VT52
    case token_vt52('G'      ) :      setAndUseCharset     (0,    'B'); break; //VT52

    case token_vt52('H'      ) : _currentScreen->setCursorYX          (1,1       ); break; //VT52
    case token_vt52('I'      ) : _currentScreen->reverseIndex         (          ); break; //VT52
    case token_vt52('J'      ) : _currentScreen->clearToEndOfScreen   (          ); break; //VT52
    case token_vt52('K'      ) : _currentScreen->clearToEndOfLine     (          ); break; //VT52
    case token_vt52('Y'      ) : _currentScreen->setCursorYX          (p-31,q-31 ); break; //VT52
    case token_vt52('Z'      ) :      reportTerminalType   (           ); break; //VT52
    case token_vt52('<'      ) :          setMode      (MODE_Ansi     ); break; //VT52
    case token_vt52('='      ) :          setMode      (MODE_AppKeyPad); break; //VT52
    case token_vt52('>'      ) :        resetMode      (MODE_AppKeyPad); break; //VT52

    case token_csi_pq('c'      ) :  reportTertiaryAttributes(          ); break; //VT420
    case token_csi_pg('c'      ) :  reportSecondaryAttributes(          ); break; //VT100

    default:
        reportDecodingError();
        break;
    }
    /* clang-format on */
}

void Vt102Emulation::clearScreenAndSetColumns(int columnCount)
{
    setImageSize(_currentScreen->getLines(), columnCount);
    clearEntireScreen();
    setDefaultMargins();
    _currentScreen->setCursorYX(0, 0);
}

void Vt102Emulation::sendString(const QByteArray &s)
{
    Q_EMIT sendData(s);
}

void Vt102Emulation::reportCursorPosition()
{
    char tmp[30];
    int y = _currentScreen->getCursorY() + 1;
    int x = _currentScreen->getCursorX() + 1;
    if (_currentScreen->getMode(MODE_Origin)) {
        y -= _currentScreen->topMargin();
    }
    snprintf(tmp, sizeof(tmp), "\033[%d;%dR", y, x);
    sendString(tmp);
}

void Vt102Emulation::reportSize()
{
    char tmp[30];
    snprintf(tmp, sizeof(tmp), "\033[8;%d;%dt", _currentScreen->getLines(), _currentScreen->getColumns());
    sendString(tmp);
}

void Vt102Emulation::reportTerminalType()
{
    // Primary device attribute response (Request was: ^[[0c or ^[[c (from TT321 Users Guide))
    // VT220:  ^[[?63;1;2;3;6;7;8c   (list deps on emul. capabilities)
    // VT100:  ^[[?1;2c
    // VT101:  ^[[?1;0c
    // VT102:  ^[[?6v
    if (getMode(MODE_Ansi)) {
        sendString("\033[?1;2c"); // I'm a VT100
    } else {
        sendString("\033/Z"); // I'm a VT52
    }
}

void Vt102Emulation::reportTertiaryAttributes()
{
    // Tertiary device attribute response DECRPTUI (Request was: ^[[=0c or ^[[=c)
    // 7E4B4445 is hex for ASCII "~KDE"
    sendString("\033P!|7E4B4445\033\\");
}

void Vt102Emulation::reportSecondaryAttributes()
{
    // Secondary device attribute response (Request was: ^[[>0c or ^[[>c)
    if (getMode(MODE_Ansi)) {
        sendString("\033[>0;115;0c"); // Why 115?  ;)
    } else {
        sendString("\033/Z"); // FIXME I don't think VT52 knows about it but kept for
    }
    // konsoles backward compatibility.
}

/* DECREPTPARM – Report Terminal Parameters
    ESC [ <sol>; <par>; <nbits>; <xspeed>; <rspeed>; <clkmul>; <flags> x

    https://vt100.net/docs/vt100-ug/chapter3.html
*/
void Vt102Emulation::reportTerminalParms(int p)
{
    char tmp[100];
    /*
       sol=1: This message is a request; report in response to a request.
       par=1: No parity set
       nbits=1: 8 bits per character
       xspeed=112: 9600
       rspeed=112: 9600
       clkmul=1: The bit rate multiplier is 16.
       flags=0: None
    */
    snprintf(tmp, sizeof(tmp), "\033[%d;1;1;112;112;1;0x", p); // not really true.
    sendString(tmp);
}

void Vt102Emulation::reportStatus()
{
    sendString("\033[0n"); // VT100. Device status report. 0 = Ready.
}

void Vt102Emulation::reportAnswerBack()
{
    // FIXME - Test this with VTTEST
    // This is really obsolete VT100 stuff.
    const char *ANSWER_BACK = "";
    sendString(ANSWER_BACK);
}

/*!
    `cx',`cy' are 1-based.
    `cb' indicates the button pressed or released (0-2) or scroll event (4-5).

    eventType represents the kind of mouse action that occurred:
        0 = Mouse button press
        1 = Mouse drag
        2 = Mouse button release
*/

void Vt102Emulation::sendMouseEvent(int cb, int cx, int cy, int eventType)
{
    if (cx < 1 || cy < 1) {
        return;
    }

    // Don't send move/drag events if only press and release requested
    if (eventType == 1 && getMode(MODE_Mouse1000)) {
        return;
    }

    if (cb == 3 && getMode(MODE_Mouse1002)) {
        return;
    }

    // With the exception of the 1006 mode, button release is encoded in cb.
    // Note that if multiple extensions are enabled, the 1006 is used, so it's okay to check for only that.
    if (eventType == 2 && !getMode(MODE_Mouse1006)) {
        cb = 3;
    }

    // normal buttons are passed as 0x20 + button,
    // mouse wheel (buttons 4,5) as 0x5c + button
    if (cb >= 4) {
        cb += 0x3c;
    }

    // Mouse motion handling
    if ((getMode(MODE_Mouse1002) || getMode(MODE_Mouse1003)) && eventType == 1) {
        cb += 0x20; // add 32 to signify motion event
    }
    char command[40];
    command[0] = '\0';
    // Check the extensions in decreasing order of preference. Encoding the release event above assumes that 1006 comes first.
    if (getMode(MODE_Mouse1006)) {
        snprintf(command, sizeof(command), "\033[<%d;%d;%d%c", cb, cx, cy, eventType == 2 ? 'm' : 'M');
    } else if (getMode(MODE_Mouse1015)) {
        snprintf(command, sizeof(command), "\033[%d;%d;%dM", cb + 0x20, cx, cy);
    } else if (getMode(MODE_Mouse1005)) {
        if (cx <= 2015 && cy <= 2015) {
            // The xterm extension uses UTF-8 (up to 2 bytes) to encode
            // coordinate+32, no matter what the locale is. We could easily
            // convert manually, but QString can also do it for us.
            QChar coords[2];
            coords[0] = cx + 0x20;
            coords[1] = cy + 0x20;
            QString coordsStr = QString(coords, 2);
            QByteArray utf8 = coordsStr.toUtf8();
            snprintf(command, sizeof(command), "\033[M%c%s", cb + 0x20, utf8.constData());
        }
    } else if (cx <= 223 && cy <= 223) {
        snprintf(command, sizeof(command), "\033[M%c%c%c", cb + 0x20, cx + 0x20, cy + 0x20);
    }

    sendString(command);
}

/**
 * The focus change event can be used by Vim (or other terminal applications)
 * to recognize that the konsole window has changed focus.
 * The escape sequence is also used by iTerm2.
 * Vim needs the following plugin to be installed to convert the escape
 * sequence into the FocusLost/FocusGained autocmd:
 * https://github.com/sjl/vitality.vim
 */
void Vt102Emulation::focusChanged(bool focused)
{
    if (_reportFocusEvents) {
        sendString(focused ? "\033[I" : "\033[O");
    }
}

void Vt102Emulation::sendText(const QString &text)
{
    if (!text.isEmpty()) {
        QKeyEvent event(QEvent::KeyPress, 0, Qt::NoModifier, text);
        sendKeyEvent(&event); // expose as a big fat keypress event
    }
}

void Vt102Emulation::sendKeyEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    KeyboardTranslator::States states = KeyboardTranslator::NoState;

    TerminalDisplay *currentView = _currentScreen->currentTerminalDisplay();
    bool isReadOnly = false;
    if (currentView != nullptr && currentView->sessionController() != nullptr) {
        isReadOnly = currentView->sessionController()->isReadOnly();
    }

    // get current states
    if (getMode(MODE_NewLine)) {
        states |= KeyboardTranslator::NewLineState;
    }
    if (getMode(MODE_Ansi)) {
        states |= KeyboardTranslator::AnsiState;
    }
    if (getMode(MODE_AppCuKeys)) {
        states |= KeyboardTranslator::CursorKeysState;
    }
    if (getMode(MODE_AppScreen)) {
        states |= KeyboardTranslator::AlternateScreenState;
    }
    if (getMode(MODE_AppKeyPad) && ((modifiers & Qt::KeypadModifier) != 0U)) {
        states |= KeyboardTranslator::ApplicationKeypadState;
    }

    if (!isReadOnly) {
        // check flow control state
        if ((modifiers & Qt::ControlModifier) != 0U) {
            switch (event->key()) {
            case Qt::Key_S:
                Q_EMIT flowControlKeyPressed(true);
                break;
            case Qt::Key_Q:
            case Qt::Key_C: // cancel flow control
                Q_EMIT flowControlKeyPressed(false);
                break;
            }
        }
    }

    // look up key binding
    if (_keyTranslator != nullptr) {
        KeyboardTranslator::Entry entry = _keyTranslator->findEntry(event->key(), modifiers, states);

        // send result to terminal
        QByteArray textToSend;

        // special handling for the Alt (aka. Meta) modifier.  pressing
        // Alt+[Character] results in Esc+[Character] being sent
        // (unless there is an entry defined for this particular combination
        //  in the keyboard modifier)
        const bool wantsAltModifier = ((entry.modifiers() & entry.modifierMask() & Qt::AltModifier) != 0U);
        const bool wantsMetaModifier = ((entry.modifiers() & entry.modifierMask() & Qt::MetaModifier) != 0U);
        const bool wantsAnyModifier = ((entry.state() & entry.stateMask() & KeyboardTranslator::AnyModifierState) != 0);

        if (((modifiers & Qt::AltModifier) != 0U) && !(wantsAltModifier || wantsAnyModifier) && !event->text().isEmpty()) {
            textToSend.prepend("\033");
        }
        if (((modifiers & Qt::MetaModifier) != 0U) && !(wantsMetaModifier || wantsAnyModifier) && !event->text().isEmpty()) {
            textToSend.prepend("\030@s");
        }

        if (entry.command() != KeyboardTranslator::NoCommand) {
            if ((entry.command() & KeyboardTranslator::EraseCommand) != 0) {
                textToSend += eraseChar();
            }
            if (currentView != nullptr) {
                if ((entry.command() & KeyboardTranslator::ScrollPageUpCommand) != 0) {
                    currentView->scrollScreenWindow(ScreenWindow::ScrollPages, -1);
                } else if ((entry.command() & KeyboardTranslator::ScrollPageDownCommand) != 0) {
                    currentView->scrollScreenWindow(ScreenWindow::ScrollPages, 1);
                } else if ((entry.command() & KeyboardTranslator::ScrollLineUpCommand) != 0) {
                    currentView->scrollScreenWindow(ScreenWindow::ScrollLines, -1);
                } else if ((entry.command() & KeyboardTranslator::ScrollLineDownCommand) != 0) {
                    currentView->scrollScreenWindow(ScreenWindow::ScrollLines, 1);
                } else if ((entry.command() & KeyboardTranslator::ScrollUpToTopCommand) != 0) {
                    currentView->scrollScreenWindow(ScreenWindow::ScrollLines, -currentView->screenWindow()->currentLine());
                } else if ((entry.command() & KeyboardTranslator::ScrollDownToBottomCommand) != 0) {
                    currentView->scrollScreenWindow(ScreenWindow::ScrollLines, lineCount());
                }
            }
        } else if (!entry.text().isEmpty()) {
            textToSend += entry.text(true, modifiers);
        } else {
            Q_ASSERT(_codec);
            textToSend += _codec->fromUnicode(event->text());
        }

        if (!isReadOnly) {
            Q_EMIT sendData(textToSend);
        }
    } else {
        if (!isReadOnly) {
            // print an error message to the terminal if no key translator has been
            // set
            QString translatorError = i18n(
                "No keyboard translator available.  "
                "The information needed to convert key presses "
                "into characters to send to the terminal "
                "is missing.");
            reset();
            receiveData(translatorError.toLatin1().constData(), translatorError.count());
        }
    }
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                VT100 Charsets                             */
/*                                                                           */
/* ------------------------------------------------------------------------- */

// Character Set Conversion ------------------------------------------------ --

/*
   The processing contains a VT100 specific code translation layer.
   It's still in use and mainly responsible for the line drawing graphics.

   These and some other glyphs are assigned to codes (0x5f-0xfe)
   normally occupied by the latin letters. Since this codes also
   appear within control sequences, the extra code conversion
   does not permute with the tokenizer and is placed behind it
   in the pipeline. It only applies to tokens, which represent
   plain characters.

   This conversion it eventually continued in TerminalDisplay.C, since
   it might involve VT100 enhanced fonts, which have these
   particular glyphs allocated in (0x00-0x1f) in their code page.
*/

#define CHARSET _charset[_currentScreen == _screen[1]]

// Apply current character map.

unsigned int Vt102Emulation::applyCharset(uint c)
{
    if (CHARSET.graphic && 0x5f <= c && c <= 0x7e) {
        return vt100_graphics[c - 0x5f];
    }
    if (CHARSET.pound && c == '#') {
        return 0xa3; // This mode is obsolete
    }
    return c;
}

/*
   "Charset" related part of the emulation state.
   This configures the VT100 charset filter.

   While most operation work on the current _screen,
   the following two are different.
*/

void Vt102Emulation::resetCharset(int scrno)
{
    _charset[scrno].cu_cs = 0;
    qstrncpy(_charset[scrno].charset, "BBBB", 4);
    _charset[scrno].sa_graphic = false;
    _charset[scrno].sa_pound = false;
    _charset[scrno].graphic = false;
    _charset[scrno].pound = false;
}

void Vt102Emulation::setCharset(int n, int cs) // on both screens.
{
    _charset[0].charset[n & 3] = cs;
    useCharset(_charset[0].cu_cs);
    _charset[1].charset[n & 3] = cs;
    useCharset(_charset[1].cu_cs);
}

void Vt102Emulation::setAndUseCharset(int n, int cs)
{
    CHARSET.charset[n & 3] = cs;
    useCharset(n & 3);
}

void Vt102Emulation::useCharset(int n)
{
    CHARSET.cu_cs = n & 3;
    CHARSET.graphic = (CHARSET.charset[n & 3] == '0');
    CHARSET.pound = (CHARSET.charset[n & 3] == 'A'); // This mode is obsolete
}

void Vt102Emulation::setDefaultMargins()
{
    _screen[0]->setDefaultMargins();
    _screen[1]->setDefaultMargins();
}

void Vt102Emulation::setMargins(int t, int b)
{
    _screen[0]->setMargins(t, b);
    _screen[1]->setMargins(t, b);
}

void Vt102Emulation::saveCursor()
{
    CHARSET.sa_graphic = CHARSET.graphic;
    CHARSET.sa_pound = CHARSET.pound; // This mode is obsolete
    // we are not clear about these
    // sa_charset = charsets[cScreen->_charset];
    // sa_charset_num = cScreen->_charset;
    _currentScreen->saveCursor();
}

void Vt102Emulation::restoreCursor()
{
    CHARSET.graphic = CHARSET.sa_graphic;
    CHARSET.pound = CHARSET.sa_pound; // This mode is obsolete
    _currentScreen->restoreCursor();
}

/* ------------------------------------------------------------------------- */
/*                                                                           */
/*                                Mode Operations                            */
/*                                                                           */
/* ------------------------------------------------------------------------- */

/*
   Some of the emulations state is either added to the state of the screens.

   This causes some scoping problems, since different emulations choose to
   located the mode either to the current _screen or to both.

   For strange reasons, the extend of the rendition attributes ranges over
   all screens and not over the actual _screen.

   We decided on the precise precise extend, somehow.
*/

// "Mode" related part of the state. These are all booleans.

void Vt102Emulation::resetModes()
{
    // MODE_Allow132Columns is not reset here
    // to match Xterm's behavior (see Xterm's VTReset() function)

    // MODE_Mouse1007 (Alternate Scrolling) is not reset here, to maintain
    // the profile alternate scrolling property after reset() is called, which
    // makes more sense; also this matches XTerm behavior.

    resetMode(MODE_132Columns);
    saveMode(MODE_132Columns);
    resetMode(MODE_Mouse1000);
    saveMode(MODE_Mouse1000);
    resetMode(MODE_Mouse1001);
    saveMode(MODE_Mouse1001);
    resetMode(MODE_Mouse1002);
    saveMode(MODE_Mouse1002);
    resetMode(MODE_Mouse1003);
    saveMode(MODE_Mouse1003);
    resetMode(MODE_Mouse1005);
    saveMode(MODE_Mouse1005);
    resetMode(MODE_Mouse1006);
    saveMode(MODE_Mouse1006);
    resetMode(MODE_Mouse1015);
    saveMode(MODE_Mouse1015);
    resetMode(MODE_BracketedPaste);
    saveMode(MODE_BracketedPaste);

    resetMode(MODE_AppScreen);
    saveMode(MODE_AppScreen);
    resetMode(MODE_AppCuKeys);
    saveMode(MODE_AppCuKeys);
    resetMode(MODE_AppKeyPad);
    saveMode(MODE_AppKeyPad);
    resetMode(MODE_NewLine);
    setMode(MODE_Ansi);
}

void Vt102Emulation::setMode(int m)
{
    _currentModes.mode[m] = true;
    switch (m) {
    case MODE_132Columns:
        if (getMode(MODE_Allow132Columns)) {
            clearScreenAndSetColumns(132);
        } else {
            _currentModes.mode[m] = false;
        }
        break;
    case MODE_Mouse1000:
    case MODE_Mouse1001:
    case MODE_Mouse1002:
    case MODE_Mouse1003:
        _currentModes.mode[MODE_Mouse1000] = false;
        _currentModes.mode[MODE_Mouse1001] = false;
        _currentModes.mode[MODE_Mouse1002] = false;
        _currentModes.mode[MODE_Mouse1003] = false;
        _currentModes.mode[m] = true;
        Q_EMIT programRequestsMouseTracking(true);
        break;
    case MODE_Mouse1007:
        Q_EMIT enableAlternateScrolling(true);
        break;
    case MODE_Mouse1005:
    case MODE_Mouse1006:
    case MODE_Mouse1015:
        _currentModes.mode[MODE_Mouse1005] = false;
        _currentModes.mode[MODE_Mouse1006] = false;
        _currentModes.mode[MODE_Mouse1015] = false;
        _currentModes.mode[m] = true;
        break;

    case MODE_BracketedPaste:
        Q_EMIT programBracketedPasteModeChanged(true);
        break;

    case MODE_AppScreen:
        _screen[1]->setDefaultRendition();
        _screen[1]->clearSelection();
        setScreen(1);
        break;
    }
    // FIXME: Currently this has a redundant condition as MODES_SCREEN is 6
    // and MODE_NewLine is 5
    if (m < MODES_SCREEN || m == MODE_NewLine) {
        _screen[0]->setMode(m);
        _screen[1]->setMode(m);
    }
}

void Vt102Emulation::resetMode(int m)
{
    _currentModes.mode[m] = false;
    switch (m) {
    case MODE_132Columns:
        if (getMode(MODE_Allow132Columns)) {
            clearScreenAndSetColumns(80);
        }
        break;
    case MODE_Mouse1000:
    case MODE_Mouse1001:
    case MODE_Mouse1002:
    case MODE_Mouse1003:
        // Same behavior as xterm, these modes are mutually exclusive,
        // and disabling any disables mouse tracking.
        _currentModes.mode[MODE_Mouse1000] = false;
        _currentModes.mode[MODE_Mouse1001] = false;
        _currentModes.mode[MODE_Mouse1002] = false;
        _currentModes.mode[MODE_Mouse1003] = false;
        Q_EMIT programRequestsMouseTracking(false);
        break;
    case MODE_Mouse1007:
        Q_EMIT enableAlternateScrolling(false);
        break;

    case MODE_BracketedPaste:
        Q_EMIT programBracketedPasteModeChanged(false);
        break;

    case MODE_AppScreen:
        _screen[0]->clearSelection();
        setScreen(0);
        break;
    }
    // FIXME: Currently this has a redundant condition as MODES_SCREEN is 7
    // MODE_AppScreen is 6 and MODE_NewLine is 5
    if (m < MODES_SCREEN || m == MODE_NewLine) {
        _screen[0]->resetMode(m);
        _screen[1]->resetMode(m);
    }
}

void Vt102Emulation::saveMode(int m)
{
    _savedModes.mode[m] = _currentModes.mode[m];
}

void Vt102Emulation::restoreMode(int m)
{
    if (_savedModes.mode[m]) {
        setMode(m);
    } else {
        resetMode(m);
    }
}

bool Vt102Emulation::getMode(int m)
{
    return _currentModes.mode[m];
}

char Vt102Emulation::eraseChar() const
{
    KeyboardTranslator::Entry entry = _keyTranslator->findEntry(Qt::Key_Backspace, Qt::NoModifier, KeyboardTranslator::NoState);
    if (entry.text().count() > 0) {
        return entry.text().at(0);
    } else {
        return '\b';
    }
}

// return contents of the scan buffer
static QString hexdump2(uint *s, int len)
{
    int i;
    char dump[128];
    QString returnDump;

    for (i = 0; i < len; i++) {
        if (s[i] == '\\') {
            snprintf(dump, sizeof(dump), "%s", "\\\\");
        } else if ((s[i]) > 32 && s[i] < 127) {
            snprintf(dump, sizeof(dump), "%c", s[i]);
        } else if (s[i] == 0x1b) {
            snprintf(dump, sizeof(dump), "ESC");
        } else {
            snprintf(dump, sizeof(dump), "\\%04x(hex)", s[i]);
        }
        returnDump.append(QLatin1String(dump));
    }
    return returnDump;
}

void Vt102Emulation::reportDecodingError()
{
    if (tokenBufferPos == 0 || (tokenBufferPos == 1 && (tokenBuffer[0] & 0xff) >= 32)) {
        return;
    }

    QString outputError = QStringLiteral("Undecodable sequence: ");
    outputError.append(hexdump2(tokenBuffer, tokenBufferPos));
}
