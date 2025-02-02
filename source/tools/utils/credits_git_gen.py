#!/usr/bin/env python3

# ##### BEGIN GPL LICENSE BLOCK #####
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software Foundation,
#  Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
# ##### END GPL LICENSE BLOCK #####

# <pep8 compliant>

"""
Example use:

   credits_git_gen.py --source=/src/blender --range=SHA1..HEAD
"""

from git_log import GitCommit, GitCommitIter
import unicodedata as ud

# -----------------------------------------------------------------------------
# Lookup Table to clean up the credits
#
# This is a combination of unifying git logs as well as
# name change requested by the authors.

AuthorLookup = {
    "Aaron": "Aaron Carlisle",
    "Your Name": "Aaron Carlisle",
    "Alan": "Alan Troth",
    "andreas atteneder": "Andreas Atteneder",
    "Ankit": "Ankit Meel",
    "Antonioya": "Antonio Vazquez",
    "Antonio  Vazquez": "Antonio Vazquez",
    "Antony Ryakiotakis": "Antony Riakiotakis",
    "bastien": "Bastien Montagne",
    "mont29": "Bastien Montagne",
    "bjornmose": "Bjorn Mose",
    "meta-androcto": "Brendon Murphy",
    "Brecht van Lommel": "Brecht Van Lommel",
    "Brecht Van Lömmel": "Brecht Van Lommel",
    "ClÃ©ment Foucault": "Clément Foucault",
    "Clément": "Clément Foucault",
    "fclem": "Clément Foucault",
    "christian brinkmann": "Christian Brinkmann",
    "ZanQdo": "Daniel Salazar",
    "unclezeiv": "Davide Vercelli",
    "gaiaclary": "Gaia Clary",
    "Diego Hernan Borghetti": "Diego Borghetti",
    "Dotsnov Valentin": "Dontsov Valentin",
    "Eitan": "Eitan Traurig",
    "EitanSomething": "Eitan Traurig",
    "Germano": "Germano Cavalcante",
    "Germano Cavalcantemano-wii": "Germano Cavalcante",
    "mano-wii": "Germano Cavalcante",
    "gsr": "Guillermo S. Romero",
    "howardt": "Howard Trickey",
    "Inês Almeida": "Ines Almeida",
    "brita": "Ines Almeida",
    "Ivan": "Ivan Perevala",
    "jensverwiebe": "Jens Verwiebe",
    "julianeisel": "Julian Eisel",
    "Severin": "Julian Eisel",
    "Alex Strand": "Kenzie Strand",
    "Kevin Dietrich": "Kévin Dietrich",
    "Mikhail": "Mikhail Matrosov",
    "lazydodo": "Ray Molenkamp",
    "Ray molenkamp": "Ray Molenkamp",
    "Author Name": "Robert Guetzkow",
    "Sybren A. StÃÂ¼vel": "Sybren A. Stüvel",
    "Simon": "Simon G",
    "Stephan": "Stephan Seitz",
    "blender": "Sergey Sharybin",
    "Vuk GardaÅ¡eviÄ": "Vuk Gardašević",
    "ianwill": "Willian Padovani Germano",
    }


# -----------------------------------------------------------------------------
# Class for generating credits

class CreditUser:
    __slots__ = (
        "commit_total",
        "year_min",
        "year_max",
    )

    def __init__(self):
        self.commit_total = 0


class Credits:
    __slots__ = (
        "users",
    )

    def __init__(self):
        self.users = {}

    def process_commit(self, c):
        # Normalize author string into canonical form, prevents duplicate credit users
        author = ud.normalize('NFC', c.author)
        author = AuthorLookup.get(author, author)
        year = c.date.year
        cu = self.users.get(author)
        if cu is None:
            cu = self.users[author] = CreditUser()
            cu.year_min = year
            cu.year_max = year

        cu.commit_total += 1
        cu.year_min = min(cu.year_min, year)
        cu.year_max = max(cu.year_max, year)

    def process(self, commit_iter):
        for i, c in enumerate(commit_iter):
            self.process_commit(c)
            if not (i % 100):
                print(i)

    def write(self, filepath,
              is_main_credits=True,
              contrib_companies=()):

        # patch_word = "patch", "patches"
        commit_word = "commit", "commits"

        with open(filepath, 'w', encoding="ascii", errors='xmlcharrefreplace') as file:
            file.write("<h3>Individual Contributors</h3>\n\n")
            for author, cu in sorted(self.users.items()):
                file.write("%s, %d %s %s<br />\n" %
                           (author,
                            cu.commit_total,
                            commit_word[cu.commit_total > 1],
                            ("" if not is_main_credits else (
                             ("- %d" % cu.year_min) if cu.year_min == cu.year_max else
                             ("(%d - %d)" % (cu.year_min, cu.year_max))))))
            file.write("\n\n")

            # -------------------------------------------------------------------------
            # Companies, hard coded
            if is_main_credits:
                file.write("<h3>Contributions from Companies & Organizations</h3>\n")
                file.write("<p>\n")
                for line in contrib_companies:
                    file.write("%s<br />\n" % line)
                file.write("</p>\n")

                import datetime
                now = datetime.datetime.now()
                fn = __file__.split("\\")[-1].split("/")[-1]
                file.write("<p><center><i>Generated by '%s' %d/%d/%d</i></center></p>\n" %
                           (fn, now.year, now.month, now.day))


def argparse_create():
    import argparse

    # When --help or no args are given, print this help
    usage_text = "Review revisions."

    epilog = "This script is used to generate credits"

    parser = argparse.ArgumentParser(description=usage_text, epilog=epilog)

    parser.add_argument(
        "--source", dest="source_dir",
        metavar='PATH', required=True,
        help="Path to git repository")
    parser.add_argument(
        "--range", dest="range_sha1",
                        metavar='SHA1_RANGE', required=True,
                        help="Range to use, eg: 169c95b8..HEAD")

    return parser


def main():

    # ----------
    # Parse Args

    args = argparse_create().parse_args()

    def is_credit_commit_valid(c):
        ignore_dir = (
            b"blender/extern/",
            b"blender/intern/opennl/",
        )

        if not any(f for f in c.files if not f.startswith(ignore_dir)):
            return False

        return True

    # TODO, there are for sure more companies then are currently listed.
    # 1 liners for in html syntax
    contrib_companies = (
        "<b>Unity Technologies</b> - FBX Exporter",
        "<b>BioSkill GmbH</b> - H3D compatibility for X3D Exporter, "
        "OBJ Nurbs Import/Export",
        "<b>AutoCRC</b> - Improvements to fluid particles, vertex color baking",
        "<b>Adidas</b> - Principled BSDF shader in Cycles",
        "<b>AMD</b> - Cycles OpenCL rendering",
        "<b>Intel</b> - Cycles ray-tracing optimization",
        "<b>NVidia</b> - Cycles Optix rendering, USD importer",
    )

    credits = Credits()
    # commit_range = "HEAD~10..HEAD"
    # commit_range = "blender-v2.81-release..blender-v2.82-release"
    # commit_range = "blender-v2.82-release"
    commit_range = args.range_sha1
    citer = GitCommitIter(args.source_dir, commit_range)
    credits.process((c for c in citer if is_credit_commit_valid(c)))
    credits.write("credits.html",
                  is_main_credits=True,
                  contrib_companies=contrib_companies)
    print("Written: credits.html")


if __name__ == "__main__":
    main()
