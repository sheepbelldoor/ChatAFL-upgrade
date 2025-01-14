from pydantic import BaseModel
from openai import OpenAI
from typing import List, Dict
from utility import utility
from enum import Enum

import os
import argparse
import json
from pprint import pprint

global ARGS, PROTOCOL, INPUT, OUTPUT, FILE_PATH

MODEL = "gpt-4o-mini"

client = OpenAI()

## Class
class Section(BaseModel):
    section_name: str
    byte_length: str
    subsection: List["Section"]

    def display_tree(self, indent: int = 0) -> str:
        # 기본 정보 출력
        tree_str = " " * indent + f"|-- {self.section_name} (Length: {self.byte_length})\n"
        # 하위 섹션이 있는 경우 재귀적으로 탐색
        for sub in self.subsection:
            tree_str += sub.display_tree(indent + 4)  # 들여쓰기 레벨 추가
        return tree_str
Section.model_rebuild() # This is required to enable recursive types

class ProtocolStructure(BaseModel):
    protocol_structure: Section

    def display_tree(self) -> str:
        return self.protocol_structure.display_tree()

## CLASS
class BinarySection(BaseModel):
    section_name: str
    byte_sequence: str
    subsection: List["BinarySection"]

    def display_tree(self, indent: int = 0) -> str:
        # 기본 정보 출력
        tree_str = " " * indent + f"|-- {self.section_name} (Byte sequence: {self.byte_sequence})\n"
        # 하위 섹션이 있는 경우 재귀적으로 탐색
        for sub in self.subsection:
            tree_str += sub.display_tree(indent + 4)  # 들여쓰기 레벨 추가
        return tree_str
BinarySection.model_rebuild() # This is required to enable recursive types

class Message(BaseModel):
    protocol_structure: BinarySection
    def display_tree(self) -> str:
        return self.protocol_structure.display_tree()

## Helper Function
def structure_to_json(structure: ProtocolStructure) -> Dict:
    ## Helper Function
    def section_to_dict(section: Section) -> Dict:
        # 현재 섹션을 'section_name': 'byte_sequence' 형식으로 변환
        section_dict = {section.section_name: section.byte_length}
        # 하위 섹션이 있을 경우 재귀적으로 변환 후 추가
        if section.subsection:
            section_dict["subsection"] = [section_to_dict(sub) for sub in section.subsection]
        return section_dict

    # Message 객체의 protocol_structure를 JSON으로 변환
    return section_to_dict(structure.protocol_structure)

def message_to_json(message: Message) -> Dict:        
    ## Helper Function
    def section_to_dict(section: BinarySection) -> Dict:
        # 현재 섹션을 'section_name': 'byte_sequence' 형식으로 변환
        section_dict = {section.section_name: section.byte_sequence}
        # 하위 섹션이 있을 경우 재귀적으로 변환 후 추가
        if section.subsection:
            section_dict["subsection"] = [section_to_dict(sub) for sub in section.subsection]
        else:
            section_dict["subsection"] = []
        return section_dict
        
    # Message 객체의 protocol_structure를 JSON으로 변환
    return section_to_dict(message.protocol_structure)

## LLM
## 1. 프로토콜의 기본 구조를 가져오는 함수
def get_protocol_structure_recursive(protocol: str) -> List[str]:
    # Prompt
    """
    For DICOM protocol, protocol message's structure is
    [("PDU Header", length=6 bytes, 
        subsection=[("PDU Type", length=1 byte),
                    ("Reserved", length=1 byte),
                    ("Length", length=4 bytes)]),
    ("PDU Data", length=variable,
        subsection=[("Data Elements", length=variable)])]
    For the {PROTOCOL} protocol, network protocol message's structure is: 
    """
    prompt = f"For DICOM protocol, protocol message's structure is "\
        "[(\"PDU Header\", length=6 bytes, subsection=[(\"PDU Type\", length=1 byte), (\"Reserved\", length=1 byte), (\"Length\", length=4 bytes)]), "\
        "(\"PDU Data\", length=variable, subsection=[(\"Data Elements\", length=variable)])]."\
        f"For the {PROTOCOL} protocol, protocol message's structure is: "

    temperature = 0.1
    completion = client.beta.chat.completions.parse(
        model=MODEL,
        temperature=temperature, # 0.1~0.3 사이로 하는 게 좋아보임
        messages=[
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": prompt}
        ],
        response_format=ProtocolStructure,
        timeout=15
    )
    response = completion.choices[0].message.parsed
    
    # Save result using utility function
    utility.save_and_log_result(
        file_path=FILE_PATH,
        model=MODEL,
        temperature=temperature,
        prompt=prompt,
        completion=completion,
        response=response
    )
    
    return structure_to_json(response)


## 2. 프로토콜 메시지가 가질 수 있는 타입을 반환하는 함수
def get_protocol_types(protocol: str) -> List[str]:
    class ProtocolType(BaseModel):
        protocol_type_list: List[str]

    # Prompt
    """
    For the DICOM protocol, protocol client request message types include ['A-ASSOCIATE-RQ', 'C-ECHO-RQ', ...].
    For the {PROTOCOL} protocol, all protocol client request message types are:
    """
    prompt = f"For the DICOM protocol, protocol client request message types include "\
            f"[\'A-ASSOCIATE-RQ\', \'A-RELEASE-RQ\', \'C-ECHO-RQ\', \'C-ECHO-RSP\', ...]."\
            f"For the {PROTOCOL} protocol, all protocol client request message types are:"

    temperature = 0.1
    completion = client.beta.chat.completions.parse(
        model=MODEL,
        temperature=temperature, # 0.1~0.3 사이로 하는 게 좋아보임
        messages=[
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": prompt}
        ],
        response_format=ProtocolType,
        timeout=15
    )
    # Parsing response
    response = completion.choices[0].message.parsed

    # Save result using utility function
    utility.save_and_log_result(
        file_path=FILE_PATH,
        model=MODEL,
        temperature=temperature,
        prompt=prompt,
        completion=completion,
        response=response
    )

    return response.protocol_type_list

## 3. 프로토콜 메시지의 특정 타입에 대한 구조를 반환하는 함수
def get_specified_protocol_structure(structure, type):
    # Prompt
    """
    For the {PROTOCOL} protocol, the base protocol message structure is {PROTOCOL STRUCTURE}.
    A specialized protocol message structure for the message type {TYPE} based on this structure is:
    """
    prompt = f"For the {PROTOCOL} protocol, the base protocol message structure is {structure}. "\
            f"A specialized protocol message structure for the message type {type} based on this structure is:"

    temperature = 0.1
    completion = client.beta.chat.completions.parse(
        model=MODEL,
        temperature=temperature, # 0.1~0.3 사이로 하는 게 좋아보임
        messages=[
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": prompt}
        ],
        response_format=ProtocolStructure,
        timeout=15
    )
    response = completion.choices[0].message.parsed

    # Save result using utility function
    utility.save_and_log_result(
        file_path=FILE_PATH,
        model=MODEL,
        temperature=temperature,
        prompt=prompt,
        completion=completion,
        response=response
    )

    return structure_to_json(response)

## 4. 메시지 타입 시퀀스를 반환하는 함수
def get_message_type_sequence(types):
    class MessageTypeSequence(BaseModel):
        message_type_sequence: List[str]
    class MessageTypeSequences(BaseModel):
        message_type_sequences: List[MessageTypeSequence]
    # Prompt
    """
    Given the {PROTOCOL} protocol with client request message types {TYPES},
    generate as many client request message type sequences as possible,
    combining 2 to 5 message types to maximize state coverage.
    """
    prompt = f"Given the {PROTOCOL} protocol with client request message types {types}, "\
            "generate as many client request message type sequences as possible, "\
            "combining 3 to 6 message types to maximize state coverage."

    temperature = 0.5
    completion = client.beta.chat.completions.parse(
        model=MODEL,
        temperature=temperature,
        messages=[
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": prompt}
        ],
        response_format=MessageTypeSequences,
        timeout=30
    )
    response = completion.choices[0].message.parsed

    # Save result using utility function
    utility.save_and_log_result(
        file_path=FILE_PATH,
        model=MODEL,
        temperature=temperature,
        prompt=prompt,
        completion=completion,
        response=response
    )

    return [sequence.message_type_sequence for sequence in response.message_type_sequences]

## 5-a. 프로토콜 메시지 생성 함수 (total)
def get_structured_message(structure, type):
    # Prompt
    """
    For {PROTOCOL} protocol, the message structure with the message type {type} is {structure}.
    Generate a {type} byte sequence message according to the structure.
    Format the byte sequence output as a string with hex bytes separated by spaces, in the format '00 01 ... fe fd'.
    Message's byte sequences are MUST reveal message type."
    """
    example_structure = {
        'SSH Protocol Message Structure': '6 bytes',
        'subsection': [
            {'PDU Header': '6 bytes', 
             'subsection': [
                {'PDU Type': '1 byte'},
                {'Reserved': '1 byte'},
                {'Length': '4 bytes'}
             ]},
            {'SSH_MSG_DISCONNECT': 'variable',
             'subsection': [
                {'Reason Code': '4 bytes'},
                {'Description': 'variable'},
                {'Language Tag': 'variable'}
             ]}
        ]
    }
    
    prompt = (f"For the {PROTOCOL} protocol, the message structure with the message type {type} is as follows: {structure}. "
             f"Generate a byte sequence message of type {type} according to this structure. "
             f"Format the byte sequence output as a string with hex bytes separated by spaces, in the format '00 01 ... fe fd'. "
             f"For example, the SSH_MSG_DISCONNECT Protocol Message Structure is: {example_structure}."
             # Backward CoT
             f"Generating a byte sequence message is backward process."
             f"1. SSH_MSG_DISCONNECT's Language Tag section is variable. Arbitrary, Language Tag is 'en' in text = \"65 6e\" in ASCII code. So, Language Tag is '65 6e'."
             f"2. SSH_MSG_DISCONNECT's Description section is variable. Arbitrary, Description is 'Disconnected' in text = \"44 69 73 63 6f 6e 6e 65 63 74 65 64\" in ASCII code. So, Description is '44 69 73 63 6f 6e 6e 65 63 74 65 64'."
             f"3. SSH_MSG_DISCONNECT's Reason Code section is 4 bytes. Arbitrary, Reason Code is 9 in decimal = 9 in hexadecimal. So, Reason Code is '00 00 00 09'."
             f"4. SSH_MSG_DISCONNECT's Length section is 4 bytes. Language Tag is 2 bytes, Description is 12 bytes (from 2), Reason Code is 4 bytes (from 3). So, Length is 2 + 12 + 4 = 18 in decimal => 12 in hexadecimal. So, Length is '00 00 00 12'."
             f"5. SSH_MSG_DISCONNECT's Reserved section with 1 byte set as '00'."
             f"6. SSH_MSG_DISCONNECT's PDU type is 1 with 1 byte. So, PDU Type is '01'."
             f"7. So, SSH_MSG_DISCONNECT's byte sequence is '01 00 00 12 00 00 00 09 44 69 73 63 6f 6e 6e 65 63 74 65 64 65 6e'.")

    temperature = 0.5
    completion = client.beta.chat.completions.parse(
        model=MODEL,
        temperature=temperature,
        messages=[
            {"role": "system", "content": f"You are an expert in communication protocols and data structures. "
                                            "Generate accurate and consistent byte sequence messages "
                                            "according to the given protocol and message structure. "
                                            "Base your answers solely on the provided information, "
                                            "and do not include additional assumptions or unnecessary details. "
                                            "Format the byte sequence output as per the instruction, "
                                            "displaying hex bytes separated by spaces, like '00 01 ... fe fd'."},
            {"role": "user", "content": prompt}
        ],
        response_format=Message,
        timeout=15
    )
    response = completion.choices[0].message.parsed

    # Save result using utility function
    utility.save_and_log_result(
        file_path=FILE_PATH,
        model=MODEL,
        temperature=temperature,
        prompt=prompt,
        completion=completion,
        response=response
    )

    return message_to_json(response)

## 6-a. 프로토콜 메시지 수정 함수 (total)
def get_modified_structured_message(message, structure, type):
    # Prompt
    """
    If the message {message} in the {PROTOCOL} protocol does not match the {type} format,
    please modify it to conform to the {type} structure.
    """
    example_structure = {
        'SSH Protocol Message Structure': '6 bytes',
        'subsection': [
            {'PDU Header': '6 bytes', 
             'subsection': [
                {'PDU Type': '1 byte'},
                {'Reserved': '1 byte'},
                {'Length': '4 bytes'}
             ]},
            {'SSH_MSG_DISCONNECT': 'variable',
             'subsection': [
                {'Reason Code': '4 bytes'},
                {'Description': 'variable'},
                {'Language Tag': 'variable'}
             ]}
        ]
    }

    prompt = (f"If the message {message} in the {PROTOCOL} protocol does not match the format for type '{type}', "
            f"which is defined as {structure}, please modify or fix it to conform to the type {type} structure."
            f"For example, the SSH_MSG_DISCONNECT Protocol Message Structure is: {example_structure}."
            f"and the SSH_MSG_DISCONNECT message is: '01 00 00 12 00 00 00 09 44 69 73 63 6f 6e 6e 65 63 74 65 64 65 6e'."
            f"1. SSH_MSG_DISCONNECT's PDU type is 1. So, It is correct."
            f"2. SSH_MSG_DISCONNECT's Reserved section with 1 byte set as '00'. So, It is correct."
            f"3. SSH_MSG_DISCONNECT's Length is 18 in decimal = 12 in hexadecimal. So, Reason Code 4 bytes, Description 12 bytes, Language Tag 2 bytes, Total 18 bytes. So, Length is '00 00 00 12'. It is correct."
            f"4. SSH_MSG_DISCONNECT's Reason Code is 9 in decimal = 9 in hexadecimal. It is correct."
            f"5. SSH_MSG_DISCONNECT's Description section 'Disconnected' in text = \"44 69 73 63 6f 6e 6e 65 63 74 65 64\" in ASCII code. So, It is correct."
            f"6. SSH_MSG_DISCONNECT's Language Tag section is variable. Language Tag is 'en' in text = \"65 6e\" in ASCII code. So, It is correct."
            f"7. Finally, the SSH_MSG_DISCONNECT message is correct. There is no need to modify it.")

    temperature = 0.5
    completion = client.beta.chat.completions.parse(
        model=MODEL,
        temperature=temperature,
        messages=[
            {"role": "system", "content": "You are an expert in communication protocols and data formatting. "
                                            "When given a message and a protocol's message structure, "
                                            "accurately modify the message to conform to the specified format. "
                                            "Base your response solely on the provided information, "
                                            "without adding any assumptions or unnecessary details."},
            {"role": "user", "content": prompt}
        ],
        response_format=Message,
        timeout=15
    )
    response = completion.choices[0].message.parsed

    # Save result using utility function
    utility.save_and_log_result(
        file_path=FILE_PATH,
        model=MODEL,
        temperature=temperature,
        prompt=prompt,
        completion=completion,
        response=response
    )

    return message_to_json(response)

def main():
    global ARGS, PROTOCOL, INPUT, OUTPUT, FILE_PATH
    PROTOCOL = ARGS.protocol
    INPUT = ARGS.input
    OUTPUT= ARGS.output
    FILE_PATH = utility.get_output_path(PROTOCOL)
    
    # 프로토콜 구조 겟또다제
    protocol_structure = get_protocol_structure_recursive(PROTOCOL)
    # 프로토콜 타입 겟또다제
    protocol_types = get_protocol_types(PROTOCOL)
    # 프로토콜 타입 시퀀스 겟또다제
    protocol_type_sequences = get_message_type_sequence(protocol_types)
    
    specified_protocol_structures = {}
    protocol_structured_messages = {}
    idx = 0
    for type in protocol_types:
        ## DEBUG
        # if idx == 1:
        #     break
        # 구체화된 프로토콜 구조 겟또다제
        try:
            specified_protocol_structure = get_specified_protocol_structure(protocol_structure, type)
            specified_protocol_structures[type] = specified_protocol_structure
        except Exception as e:
            print(f"Error in get_specified_protocol_structure(): {e}")
        # 프로토콜 메시지 겟또다제
        ## Case 1: 전체 메시지 한 번에 생성
        try:
            protocol_structured_message = get_structured_message(specified_protocol_structure, type)
            protocol_structured_messages[type] = protocol_structured_message
        except Exception as e:
            print(f"Error in get_structured_message(): {e}")
        # 프로토콜 메시지 수정본 겟또다제
        try:
            protocol_modified_structured_message = get_modified_structured_message(utility.concatenate_values(protocol_structured_message), specified_protocol_structure, type)
            protocol_structured_messages[type] = protocol_modified_structured_message
        except Exception as e:
            print(f"Error in get_modified_structured_message(): {e}")

        idx += 1

    pprint(specified_protocol_structures)
    pprint(protocol_structured_messages)

    # 메시지 시퀀스에 따른 시드 입력 생성
    for type_sequence in protocol_type_sequences:
        output_path = utility.get_byte_sequence_output_path(PROTOCOL, OUTPUT)
        byte_sequence = ""
        for type in type_sequence:
            if protocol_structured_messages.get(type) == None:
                try:
                    print(f"Can't make new binary message sequences in {output_path}: No structured {type} type message.")
                    os.remove(output_path)
                except Exception as e:
                    print(f"{e}")
                break
            byte_sequence += utility.concatenate_values(protocol_structured_messages.get(type))
            ## Case 1. 각 메시지마다 줄바꿈을 하여 바이너리 파일로 저장
            try:
                utility.add_byte_sequence_to_file(byte_sequence=utility.concatenate_values(protocol_structured_messages.get(type)),
                                                  file_path=output_path)
            except Exception as e:
                print(f"Error in add_byte_sequence_to_file: {e}")



if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-i', '--input', required=False, help='Input file')
    parser.add_argument('-p', '--protocol', required=True, help='Target Protocol')
    parser.add_argument('-o', '--output', required=False, default='results', help='Output Directory')
    ARGS = parser.parse_args()
    main()
